A very simple shell made with C.

Supports piping via FIFO's (only a single pipe is supported)
If you use this code and want to support piping without named pipes, refactoring to using pipe() instead should take under 5 minutes.

Custom commands implemented: History (up to 100), cd/chdir, limit (memory)
Signal handling: Ctrl Z is ignored, Ctrl c will give a prompt 

NOTE: Only tested on Linux. It works on Mac as well, but there were some issues with chdir regarding home path 


Compile with GCC 

```
gcc -Wall rayshell.c -o rayshell
```

Using named pipes 

```
mkfifo testFifo 
./rayshell testFifo 
```

File redirection is also supported and the shell will terminate at EOF

```
./rayshell < commands.txt 
```

Known bugs: 

If you do this:
```
Limit 1000 // set low memory bound
Limit 100000
cd 
```
cd will fail to find the home path 

License TLDR: Use code as you please



