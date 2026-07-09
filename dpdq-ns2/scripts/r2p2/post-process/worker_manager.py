import multiprocessing
from multiprocessing.connection import wait
from multiprocessing import Queue, Manager
import queue
from dataclasses import dataclass
from typing import Callable, Tuple, Any
import os
import time
from typing import List

"""
Quick and dirty thread pool manager. Far from ideal as a whole group must finish before the next one starts.
This module exists to make sure that not too many threads are spawned. It's a basic global thread allocator.

TODO: Create worker loops that read and execute tasks from TASK_QUEUE directly
"""
@dataclass
class Task:
    fun: Callable
    args: Tuple[Any, ...]

@dataclass
class TaskBatch:
    tasks: 'list[Task]'


def batch(tasks, batch_size):
    '''
    Returns list of TaskBatch
    '''
    return [TaskBatch(tasks[i:i+batch_size]) for i in range(0, len(tasks), batch_size)]

def add_tasks(tasks: 'list[Task]', task_family="Unknown family", suggested_batch_size=1):
    global TMP_TASK_QUEUE
    assert isinstance(tasks, List) and all(isinstance(task, Task) for task in tasks), f"tasks is not of type list[Task] but is {type(tasks)}"

    print(f"Pid: {os.getpid()} Adding {len(tasks)} {task_family} tasks to tmp task Q of len = {TMP_TASK_QUEUE.qsize()}")

    task_batches = batch(tasks, suggested_batch_size)
    for task_batch in task_batches:
        TMP_TASK_QUEUE.put(task_batch)

def get_and_remove_tasks(num_tasks):
    global TASK_QUEUE

    num_tasks = min(num_tasks, TASK_QUEUE.qsize())
    print(f"Pid: {os.getpid()} Removing {num_tasks} tasks from task Q. Current len = {TASK_QUEUE.qsize()}")
    ret = [TASK_QUEUE.get() for _ in range(num_tasks)]

    return ret

def task_mover_loop(init=False):
    '''
    Hack. Without this, child processes adding Tasks directly in TASK_QUEUE do not exit.
    See https://stackoverflow.com/questions/70255691/python-multiprocessing-child-processes-not-quiting-normally
    '''
    global TASK_QUEUE, TMP_TASK_QUEUE
    while True:
        task_batch: TaskBatch = TaskBatch([])
        try:
            task_batch = TMP_TASK_QUEUE.get(timeout=15)
        except queue.Empty:
            print(f"Found no task batches in TMP_TASK_QUEUE")

        if len(task_batch.tasks) == 1 and task_batch.tasks[0].fun == None:
            print("task_mover_loop() exiting")
            return
        TASK_QUEUE.put(task_batch)
        if init and TMP_TASK_QUEUE.qsize() == 0:
            return

def run_batch(tasks: 'list[Task]'):
    for task in tasks:
        task.fun(*task.args)

def run_loop(num_threads):
    global HW_THREADS_AVAILABLE, TASK_QUEUE
    HW_THREADS_AVAILABLE = num_threads

    task_mover_loop(init=True) # why?

    p = multiprocessing.Process(target=task_mover_loop)
    p.start()

    assert HW_THREADS_AVAILABLE >= 0, "Negative HW threads available"

    tries = 3
    while tries > 0:
        task_batches = get_and_remove_tasks(HW_THREADS_AVAILABLE)
        while len(task_batches) > 0:
            tries = 3
            print(f"Pid: {os.getpid()} Looping. Current task_batches {len(task_batches)}")
            running = []
            for task_batch in task_batches:
                p = multiprocessing.Process(target=run_batch, args=(task_batch.tasks,))
                running.append(p)
                p.start()

            for p in running:
                p.join()
            
            
            print(f"Pid: {os.getpid()} Current task_batches finished")
            task_batches = get_and_remove_tasks(HW_THREADS_AVAILABLE)

        tries -= 1
        time.sleep(1)
    
    add_tasks([Task(None, None)], "LoopStopper", suggested_batch_size=1)


TASK_QUEUE = Queue()
TMP_TASK_QUEUE = Queue()