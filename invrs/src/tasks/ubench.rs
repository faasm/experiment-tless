use clap::Args;
use crate::env::Env;
use csv::ReaderBuilder;
use indicatif::{ProgressBar, ProgressStyle};
use plotters::prelude::*;
use serde::Deserialize;
use std::{collections::BTreeMap, fmt, fs, fs::File, io::{Write, BufWriter}, path::PathBuf, process::Command, str, str::FromStr, time::Instant};

pub enum MicroBenchmarks {
    VerifyEDag,
}

impl fmt::Display for MicroBenchmarks {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MicroBenchmarks::VerifyEDag => write!(f, "verify-edag"),
        }
    }
}

// TODO: bump to 10
static MAX_NUM_CHAINS: u32 = 3;

#[derive(Debug, Args)]
pub struct UbenchRunArgs {
    // TODO: bump to 3
    #[arg(long, default_value = "1")]
    num_repeats: u32,
    #[arg(long, default_value = "0")]
    num_warmup_repeats: u32,
}

#[derive(Debug)]
pub struct Ubench {}

impl Ubench {
    fn get_progress_bar(
        num_repeats: u32,
        exp: &MicroBenchmarks,
        baseline: &str,
        mode: &str,
    ) -> ProgressBar {
        let pb = ProgressBar::new(num_repeats.into());
        pb.set_style(
            ProgressStyle::default_bar()
                .template("{msg} [{bar:40.cyan/blue}] {pos}/{len} ({percent}%)")
                .expect("invrs(eval): error creating progress bar")
                .progress_chars("#>-"),
        );
        pb.set_message(format!("{exp}/{baseline}/{mode}"));
        pb
    }

    fn run_edag_verify_ubench(run_args: &UbenchRunArgs) {
        let baselines: Vec<&str> = vec!["crypto-acc", "vanilla"];
        let mut verify_dir = Env::proj_root();
        verify_dir.push("verify");

        let modes = vec![0, 1];

        for baseline in &baselines {
            // Work-out binary path
            let mut binary_path = Env::proj_root();
            binary_path.push("verify");
            match *baseline {
                "crypto-acc" => binary_path.push("target/release/host"),
                "vanilla" => binary_path.push("target-nocrypto-acc/release/host"),
                _ => panic!("tlessctl(eval): unsupported baseline {baseline}"),
            };

            for mode in &modes {
                let mode_name = match *mode {
                    0 => "noagg",
                    1 => "agg",
                    _ => panic!("tless(eval): unrecognised mode"),
                };

                // Work-out file name
                let mut results_dir = Env::proj_root();
                results_dir.push("eval");
                results_dir.push(format!("{}", MicroBenchmarks::VerifyEDag));
                results_dir.push("data");
                fs::create_dir_all(results_dir.clone()).unwrap();
                results_dir.push(format!("{baseline}_{mode_name}.csv"));

                let mut csv_file = BufWriter::new(File::create(results_dir).unwrap());
                writeln!(csv_file, "Run,Parameter,ExecTimeMS").unwrap();

                let pb = Self::get_progress_bar(MAX_NUM_CHAINS * run_args.num_repeats, &MicroBenchmarks::VerifyEDag, baseline, mode_name);
                for param in 1..=MAX_NUM_CHAINS {
                    for run in 1..=run_args.num_repeats {
                        let start = Instant::now();
                        // Execute the baseline binary with the mode and parameter
                        println!("{:?}", binary_path.clone());
                        let output = Command::new(binary_path.clone())
                            .current_dir(verify_dir.clone())
                            .arg(mode.to_string())
                            .arg(param.to_string())
                            .output()
                            .unwrap();

                        // Ensure command executed successfully
                        match output.status.code() {
                            Some(0) => {
                                let elapsed_time = start.elapsed().as_millis();
                                writeln!(csv_file, "{},{},{:?}", run, param, elapsed_time).unwrap();
                                pb.inc(1);
                            }
                            _ => {
                                let stderr = str::from_utf8(&output.stderr).unwrap_or("tlessctl(eval): failed to get stderr");
                                eprintln!("tlessctl(eval): error running command: {}", stderr);
                            }
                        }
                    }
                }

                pb.finish();
            }
        }
    }

    fn get_all_data_files(exp: &MicroBenchmarks) -> Vec<PathBuf> {
        let data_path = format!("{}/eval/{exp}/data", Env::proj_root().display());

        // Collect all CSV files in the directory
        let mut csv_files = Vec::new();
        for entry in fs::read_dir(data_path).unwrap() {
            let entry = entry.unwrap();
            if entry.path().extension().and_then(|e| e.to_str()) == Some("csv") {
                csv_files.push(entry.path());
            }
        }

        return csv_files;
    }

    fn plot_edag_verify_ubench(data_files: &Vec<PathBuf>) {
        #[derive(Debug, Deserialize)]
        #[serde(rename_all = "PascalCase")]
        struct Record {
            #[allow(dead_code)]
            run: u32,
            parameter: u32,
            exec_time_m_s: u128,
        }

        // Use crypto-acceleration or not
        let baselines: Vec<&str> = vec!["crypto-acc", "vanilla"];
        // Aggreagate signatures or not
        let modes: Vec<&str> = vec!["agg", "noagg"];
        let agg_func_exec_time: i32 = 10;

        // one more than MAX_CHAINS
        const VEC_SIZE : usize = 4;

        // Collect data
        let mut data = BTreeMap::<&str, BTreeMap<&str, [u128; VEC_SIZE]>>::new();
        for baseline in &baselines {
            let mut inner_map = BTreeMap::<&str, [u128; VEC_SIZE]>::new();
            for mode in &modes {
                inner_map.insert(*mode, [0; VEC_SIZE]);
            }
            data.insert(*baseline, inner_map);
        }

        for (baseline, inner) in &data {
            for (mode, agg) in inner {
                println!("{baseline}/{mode}: size {}", agg.len());
            }
        }

        let mut y_max: f64 = 0.0;
        for csv_file in data_files {
            let file_name = csv_file
                .file_name()
                .and_then(|f| f.to_str())
                .unwrap_or_default();
            let file_name_len = file_name.len();
            let file_name_no_ext = &file_name[0..file_name_len - 4];
            let parts : Vec<&str> = file_name_no_ext.split("_").collect();

            let baseline: &str = parts[0];
            let mode: &str = parts[1];

            // Open the CSV and deserialize records
            let mut reader = ReaderBuilder::new()
                .has_headers(true)
                .from_path(csv_file)
                .unwrap();
            let mut count = 0;

            for result in reader.deserialize() {
                println!("{baseline}/{mode}: {csv_file:?}");
                let record: Record = result.unwrap();

                let agg_times = data
                    .get_mut(&baseline)
                    .unwrap()
                    .get_mut(&mode)
                    .unwrap();

                count += 1;
                let idx: usize = record.parameter.try_into().unwrap();
                agg_times[idx] += record.exec_time_m_s;
            }

            let num_repeats : u128 = (count / MAX_NUM_CHAINS).into();

            let average_times = data.get_mut(&baseline).unwrap().get_mut(&mode).unwrap();
            for i in 0..average_times.len() {
                average_times[i] = average_times[i] / num_repeats;

                let y_val : f64 = (average_times[i] / 1000) as f64;
                if y_val > y_max {
                    y_max = y_val;
                }
            }

        }

        let mut plot_path = Env::proj_root();
        plot_path.push("eval");
        plot_path.push(format!("{}", MicroBenchmarks::VerifyEDag));
        plot_path.push("plots");
        fs::create_dir_all(plot_path.clone()).unwrap();
        plot_path.push(format!("{}.svg", MicroBenchmarks::VerifyEDag));

        // Plot data
        let root = SVGBackend::new(&plot_path, (800, 300)).into_drawing_area();
        root.fill(&WHITE).unwrap();

        let mut chart = ChartBuilder::on(&root)
            .x_label_area_size(40)
            .y_label_area_size(40)
            .margin(10)
            .margin_top(40)
            .build_cartesian_2d(0..MAX_NUM_CHAINS, 0f64..300f64)
            .unwrap();

        chart
            .configure_mesh()
            .y_label_style(("sans-serif", 20).into_font())
            .x_desc("")
            // .x_labels(MAX_NUM_CHAINS.try_into().unwrap())
            // .x_label_formatter(&|_| format!(""))
            // .y_labels(10)
            // .disable_x_mesh()
            // .disable_x_axis()
            // .y_label_formatter(&|y| format!("{:.0}", y))
            .draw()
            .unwrap();

        // Manually draw the y-axis label with a custom font and size
        root.draw(&Text::new(
            "Slowdown (vs non-confidential)",
            (5, 260),
            ("sans-serif", 20)
                .into_font()
                .transform(FontTransform::Rotate270)
                .color(&BLACK),
        ))
        .unwrap();

        for (label, inner_data) in data {
            for (mode, values) in inner_data {
                let y_values: Vec<f64> = values.iter().map(|&x| x as f64 / 1000.0).collect();
                let x_values: Vec<u32> = (0..y_values.len() as u32).collect(); // x values from 0 to N-1

                chart.draw_series(LineSeries::new(
                    x_values.iter().zip(y_values.iter()).map(|(&x, &y)| (x, y)),
                    &Palette99::pick(0),
                )).unwrap()
                .label(mode)
                .legend(|(x, y)| {
                    PathElement::new(vec![(x, y)], &Palette99::pick(0))
                });
            }
        }

        root.present().unwrap();
    }

    pub fn run(ubench: &MicroBenchmarks, run_args: &UbenchRunArgs) {
        match ubench {
            MicroBenchmarks::VerifyEDag => Self::run_edag_verify_ubench(&run_args),
        };
    }

    pub fn plot(ubench: &MicroBenchmarks) {
        let data_files = Self::get_all_data_files(ubench);

        match ubench {
            MicroBenchmarks::VerifyEDag => Self::plot_edag_verify_ubench(&data_files),
        };
    }
}