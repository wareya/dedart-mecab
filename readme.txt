Dedart is a tool for dumping the associative array in mecab sys.dic files into plaintext TSV.

The TSV files produced by this should not actually be parsed by a TSV parser. Just split each line on
the '\t' character. No escape sequences are used nor are they necessary for the data being dumped.

The value stored under each string is an internal index used by mecab, where the upper 24 (23) bits of
it as a 32 (31) bit number store the location of the beginning of it in an array, and the lower 8 bits
of it store the number of tokens that share that key, and the key is the spelling. The key is the way
that a given token is spelled as it appears in text when it's fed to the parser. All other information
about it is found through other data in the dictionary's various files, which this tool does not dump.

Attributed to the public domain through the CC0 public domain attribution, and Unlicense.

g++ main.cpp -Wall -Wextra -pedantic -O3 -o dedart[.exe]
