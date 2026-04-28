#!/bin/bash
# Simplest possible test — write "hello" to stdout, exit.
# The test client's PTY will receive these bytes and feed them through
# the parser; the dump should show 5 cells with chars h e l l o on row 0.
printf 'hello'
