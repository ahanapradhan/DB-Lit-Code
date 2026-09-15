# LRU-K Page Replacement Simulator

A compact, reproducible Python simulator for the **LRU-K page replacement algorithm**, inspired by:

> Elizabeth J. O'Neil, Patrick E. O'Neil, and Gerhard Weikum,  
> *The LRU-K Page Replacement Algorithm for Database Disk Buffering*, SIGMOD 1993.

This project implements LRU-K with a configurable `K` value, includes LRU and LFU baselines, and provides paper-style synthetic experiments for evaluating page replacement behavior. The OLTP experiment from the paper is intentionally omitted because it depends on a legacy/system-specific setup that is not reproducible in a portable standalone simulator.


