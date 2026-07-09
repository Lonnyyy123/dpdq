import sys
import os
import cfg
import helper as hlp
import numpy as np
import matplotlib.pyplot as plt

"""
Simple script that reads csv files from results and plots the specified csv columns.
Example Usage (see usage()):
python3 plot_xy.py 0 1 <output_title> <experiment_dir_name_in results/> "<output_label>,<path_to_csv>" "<output_label>,<path_to_csv>"
"""

OUT = "tmp_plot"
LW = 2


class Options:
    identifier: str  # unique identifier that exists on the path of files of this type
    y_title: str
    x_title: str
    log_x_axis: bool
    log_y_axis: bool

    def __init__(self, identifier, y_title, x_title, log_x_axis, log_y_axis, lw=LW):
        self.identifier = identifier
        self.y_title = y_title
        self.x_title = x_title
        self.log_x_axis = log_x_axis
        self.log_y_axis = log_y_axis
        self.lw = lw


DEFAULT_OPTIONS = Options("DEFAULT", "Unspecified", "Unspecified", False, False)
SLDWN_ID = "output/app"
QTS_ID = "output/qts"
THRPT_ID = "output/qts"
CC_ID = "output/cc"

id_to_options = {
    SLDWN_ID: Options(SLDWN_ID, "Slowdown", "Msg size (KB)", True, False),
    QTS_ID: Options(QTS_ID, "Queue Size (KB)", "Time (ms)", False, False, lw=1),
    # THRPT_ID: Options(QTS_ID, "Throughput (Gbps)", "Time (ms)", False, False, lw=1),
    CC_ID: Options(CC_ID, "-", "Time (ms)", False, False, lw=1),
}


def find_options(path):
    for id in id_to_options:
        if id in path:
            return id_to_options[id]
    return DEFAULT_OPTIONS


def usage():
    print(
        "Usage: python3 process_results.py <x_column> <y_column> <title>"
        + ' <experiment-name> <space-separated list of "label,file path in data/">'
    )


def main():
    if len(sys.argv) < 6:
        usage()
        exit(1)
    x_column = int(sys.argv[1])
    y_column = int(sys.argv[2])

    base_title = sys.argv[3]
    experiment_name = sys.argv[4]
    file_list = sys.argv[5:]

    options = find_options(file_list[0])

    this_file_path = os.path.dirname(os.path.abspath(__file__))
    results_path = f"{this_file_path.split('/post-process')[0]}/coord/results"
    experiment_data_path = f"{results_path}/{experiment_name}/data"

    smoothing_windows = [1, 5, 15]

    for sw in smoothing_windows:
        fig, ax1 = plt.subplots(1, 1, figsize=(10, 5))
        title = base_title + f" - sw: {sw}"
        name = f"{title} ||| "
        for label_file in file_list:
            label = label_file.split(",")[0]
            name += f"{label} vs"
            file = label_file.split(",")[1]
            file = experiment_data_path + "/" + file

            data = np.genfromtxt(file, delimiter=",", skip_header=1)

            x = data[sw - 1 :, x_column]/1000.0  # ms
            y = hlp.moving_average(data[:, y_column], sw)

            ax1.plot(x, y, label=label, linewidth=options.lw)
            ax1.grid(True, linestyle="--", axis="y", linewidth=0.3)
            ax1.set_xlabel(options.x_title)
            # ax1.set_ylabel(options.y_title)
            ax1.legend(loc="best")
            ax1.set_title(f"{title}")
            if options.log_x_axis:
                ax1.set_xscale("log")
            if options.log_y_axis:
                ax1.set_yscale("log")
        hlp.make_dir(f"{OUT}/{experiment_name}")
        save_dir = f"{OUT}/{experiment_name}/{name}"
        plt.tight_layout()
        fig.savefig(f"{save_dir}.png", format="png")


if __name__ == "__main__":
    main()
