'''
This file holds common functionality
'''

from pathlib import Path
import shutil
import os

def import_param_file(param_file):
    '''
    Parses TCL script and returns a dict where keys are param names and 
    values are param values (can be a list). Ignores TCL dictionaries.
    '''
    res = dict()
    with open(param_file) as f:
        lines = f.readlines()
        for line in lines:
            ln = line.split()
            if ln[0] != "set":
                continue
            if "{" in ln:
                # List
                continue
            else:
                # scalar
                res[ln[1]] = [ln[2].strip('\"')]
    return res

def set_simple_tcl_var(name, value):
    return f"set {name} \"{value}\"\n"

def set_tcl_list(name, items):
    '''
    itmes must be a list
    '''
    ret = f"set {name} {{"
    for item in items:
        ret += f" {item}"
    ret += f" }}\n"
    return ret

def from_string(value: str, to_type: type, add_quotes_to_str=True, offset=0.0):
    if not isinstance(value, str):
        raise TypeError("value must be of type str")
    if to_type is str:
        if add_quotes_to_str:
            return f"\"{value}\""
        else:
            return value
    elif to_type is bool:
        return value
    elif to_type is int:
        return int(float(value) + offset)
    elif to_type is float:
        return float(value) +offset
    else:
        raise TypeError("to_type must be str or int or float")

def make_dir(path):
    try:
        os.makedirs(path, exist_ok=True)
    except OSError:
        print("Failed to create directory")

def delete_and_create_dir(dirpath: str):
    dir = Path(dirpath)
    if dir.exists() and dir.is_dir():
        shutil.rmtree(dir)
    make_dir(dirpath)
    