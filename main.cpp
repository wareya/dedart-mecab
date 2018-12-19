#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <vector>
#include <string>

struct link {
    uint32_t base;
    uint32_t check;
};

int check_valid_link(link * array, uint32_t arraylen, uint32_t from, uint32_t to)
{
    // check for overflow
    if(to >= arraylen)
        return 1;
    // make sure we didn't follow a link from somewhere we weren't supposed to
    else if(array[to].check != from)
        return 2;
    // make sure we don't follow a link back where we started
    else if(array[to].base == from)
        return 3;
    return 0;
}

int check_valid_out(link * array, uint32_t arraylen, uint32_t from, uint32_t to)
{
    int err;
    if((err = check_valid_link(array, arraylen, from, to)))
        return err;
    // don't follow links to bases that aren't outputs
    else if(array[to].base < 0x80000000)
        return -1;
    return 0;
}

struct entry {
    std::string key;
    uint32_t value;
};

void collect_down(link * array, uint32_t arraylen, uint32_t base, std::vector<entry> & collection, std::string key = "")
{
    if(check_valid_out(array, arraylen, base, base) == 0)
        collection.push_back({key, ~array[base].base});
    for(uint32_t i = 0; i <= 0x100; i++)
    {
        if(check_valid_link(array, arraylen, base, base+1+i) == 0)
            collect_down(array, arraylen, array[base+1+i].base, collection, key + char(i));
    }
}

int main(int argc, char ** argv)
{
    if(argc < 2)
        return puts("usage: ./dedart sys.dic"), 0;
    
    auto f = fopen(argv[1], "rb");
    if(!f)
        return puts("failed to open file"), 0;
    
    // location of the number of bytes used to store the dual-array trie
    fseek(f, 0x18, SEEK_SET);
    uint32_t arraybytes = 0;
    fread(&arraybytes, 4, 1, f);
    
    // location of the encoding spec string
    fseek(f, 0x28, SEEK_SET);
    
    char encoding[0x20];
    fread(encoding, 1, 0x20, f);
    
    if(strncmp(encoding, "UTF-8", 0x20) != 0)
        return puts("unsupported sys.dic encoding (only utf-8 is supported). stop using legacy encodings for infrastructure!");
    
    // the dual-array trie starts at 0x48
    // it's a fucked up associative search tree that follows links that point to tables that can be compressed together by overlapping them
    // :|
    
    fseek(f, 0x48, SEEK_SET);
    
    // we work out of memory, not mmapped files, because we're not barbarians and  ***20MB REALLY IS NOT A LOT OF MEMORY GUYS JESUS CHRIST***.
    
    link * array = (link *)malloc(arraybytes);
    uint32_t arraylen = arraybytes/sizeof(link);
    uint32_t read = fread(array, sizeof(link), arraylen, f);
    fclose(f);
    
    if(read != arraylen)
        return puts("error: dictionary is broken (couldn't find all the links we need)");
    
    // traverse the dual-array trie through brute force, searching for all possible valid links, recording the path (key) taken to each valid output link we find
    // sound crazy? it takes like 0.6ish seconds.
    
    std::vector<entry> collection;
    collect_down(array, arraylen, array[0].base, collection);
    
    for(auto & entry : collection)
    {
        printf("%s\t%u\n", entry.key.data(), entry.value);
        // escaping is unnecessary for unidic (at least non-neologd), just don't use a real TSV parser to parse the output
        /*
        for(auto & c : entry.key)
        {
            if(c == '\\')
                printf("\\\\");
            else if(c == '\t')
                printf("\\t");
            else if(c == '\n')
                printf("\\n");
            else if(c == '\r')
                printf("\\r");
            else
                printf("%c", c);
        }
        printf("\t%u\n", entry.value);
        */
    }
}