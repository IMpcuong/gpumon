#!/bin/bash

clang++ -std=c++20 -o gpu main.cpp -framework CoreFoundation -framework IOKit && ./gpu
