#!/bin/sh
clang -g -fsanitize=address -std=c23 -march=native -O0 -Wall -Wextra -Wpedantic -flto=thin -o live-notify live-notify.c -lcurl -lsystemd
clang -g -std=c23 -march=native -O3 -Wall -Wextra -Wpedantic -flto=thin -o setup-config setup-config.c
clang -g -std=c23 -march=native -O3 -Wall -Wextra -Wpedantic -flto=thin -o apply-config apply-config.c
