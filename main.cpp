#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <vector>
#include <string>
#include <map>
#include <set>

#include "include/unishim_split.hpp"
#include "include/micropather.hpp"

struct link {
    uint32_t base;
    uint32_t check;
};

struct token {
    uint16_t left_context;
    uint16_t right_context;

    uint16_t pos;
    int16_t  cost;

    uint32_t feature_offset; // offset into a byte pile

    uint32_t unused; // unused (originally meant for a type of metadata that never got added; effectively padding)
};

size_t codepoint_length(const std::string & str)
{
    size_t seen_codepoints = 0;
    unishim_callback mycallback = [](uint32_t codepoint, size_t offset, uint8_t size, UNISHIM_PUN_TYPE * userdata) -> int
    {
        (void)offset;
        (void)size;
        auto seen_codepoints = (size_t *)userdata;
        if(codepoint == 0)
            return 1;
        seen_codepoints[0] += 1;
        return 0;
    };
    auto status = utf8_iterate((uint8_t *)str.data(), 0, mycallback, &seen_codepoints);
    
    if(status != 0)
        return 0;
    
    return seen_codepoints;
}

int32_t min_token_cost_ever = 0x7FFF;
int32_t max_token_cost_ever = -0x8000;

int32_t min_edge_cost_ever = 0x7FFF;
int32_t max_edge_cost_ever = -0x8000;

struct bettertoken {
    uint16_t left_context;
    uint16_t right_context;
    uint16_t pos;
    int16_t cost;
    
    std::string surface;
    std::string feature;
    
    size_t start = 0;
    size_t length;
    
    uint8_t type = 0; // 0: normal; 1: BOS; 2: EOS; 3: UNK
    
    void assign(token & other, char * strpile, std::string from)
    {
        left_context = other.left_context;
        right_context = other.right_context;
        pos = other.pos;
        cost = other.cost;
        
        if(cost > max_token_cost_ever)
            max_token_cost_ever = cost;
        if(cost < min_token_cost_ever)
            min_token_cost_ever = cost;
        
        surface = from;
        feature = std::string(strpile + other.feature_offset);
        
        length = codepoint_length(surface.data());
    }
};

// functions for traversing the dual-array trie

int check_valid_link(const link * array, uint32_t arraylen, uint32_t from, uint32_t to)
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

int check_valid_out(const link * array, uint32_t arraylen, uint32_t from, uint32_t to)
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

void collect_down(const link * array, uint32_t arraylen, uint32_t base, std::vector<entry> & collection, const std::string key = "")
{
    if(check_valid_out(array, arraylen, base, base) == 0)
        collection.push_back({key, ~array[base].base});
    for(uint32_t i = 0; i <= 0x100; i++)
    {
        if(check_valid_link(array, arraylen, base, base+1+i) == 0)
            collect_down(array, arraylen, array[base+1+i].base, collection, key + char(i));
    }
}

// end of functions for traversing the dual-array trie

uint16_t read_u16(FILE * f)
{
    uint16_t r = 0;
    fread(&r, 2, 1, f);
    return r;
}
uint32_t read_u32(FILE * f)
{
    uint32_t r = 0;
    fread(&r, 4, 1, f);
    return r;
}

std::string read_entire_file_into_string(FILE * f, bool strip_control = true)
{
    auto start = ftell(f);
    fseek(f, 0, SEEK_END);
    auto end = ftell(f);
    auto count = end-start;
    fseek(f, start, SEEK_SET);
    auto chars = (char *)malloc(count+1);
    chars[count] = 0;
    fread(chars, 1, count, f);
    if(strip_control)
    {
        size_t i = count;
        while(uint8_t(chars[i]) < 0x20 and chars[i] != '\t')
        {
            chars[i] = 0;
            if(i == 0)
                break;
            i--;
        }
    }
    return std::string(chars);
}

void print_str_json_escaped(const std::string & str)
{
    for(auto & c : str)
    {
        if(uint8_t(c) < 0x20)
            printf("\\u%04X", uint8_t(c));
        else if(c == '"')
            printf("\\\"");
        else if(c == '\\')
            printf("\\\\");
        else
            printf("%c", c);
    }
}

struct index_building_data {
    std::vector<size_t> indexes = {};
    bool seen_null = false;
    uint8_t last_seen_size = 0;
};

// the last index contains one byte past the end of the string
std::vector<size_t> build_indexes(const std::string & str)
{
    index_building_data mydata;
    
    unishim_callback mycallback = [](uint32_t codepoint, size_t offset, uint8_t size, UNISHIM_PUN_TYPE * userdata) -> int
    {
        auto mydata = (index_building_data *)userdata;
        mydata->indexes.push_back(offset);
        mydata->last_seen_size = size;
        if(codepoint == 0)
            mydata->seen_null = true;
        return 0;
    };
    auto status = utf8_iterate((uint8_t *)str.data(), 0, mycallback, &mydata);
    
    if(status != 0)
        return {};
    
    if(!mydata.seen_null)
        mydata.indexes.push_back(mydata.indexes[mydata.indexes.size()-1]+mydata.last_seen_size);
    
    return mydata.indexes;
}

void collect_prefixes(std::string str, std::set<std::string> & prefix_collection)
{
    auto indexes = build_indexes(str);
    
    for(auto iter = indexes.rbegin(); iter != indexes.rend(); iter++)
    {
        auto end = *iter;
        if(end == 0)
            continue;
        if(end == str.size())
            continue;
        // the above two conditions also ensure the string is not empty
        while(str.size() > end)
            str.pop_back();
        prefix_collection.insert(str);
    }
}

struct World : public micropather::Graph {
    /**
    Return the least possible cost between 2 states. For example, if your pathfinding 
    is based on distance, this is simply the straight distance between 2 points on the 
    map. If you pathfinding is based on minimum time, it is the minimal travel time 
    between 2 points given the best possible terrain.
    */
    
    
    std::map<std::string, std::vector<bettertoken>> dictionary;
    std::set<std::string> contains_longer;
    
    uint32_t left_edges;
    uint32_t right_edges;
    int16_t * connection_matrix;
    
    std::vector<std::vector<bettertoken>> lattice;
    bettertoken dummy_beginning;
    bettertoken dummy_ending;
    
    float LeastCostEstimate(void * stateStart, void * stateEnd)
    {
        const auto start = (bettertoken *)stateStart;
        const auto end   = (bettertoken *)stateEnd;
        const auto start_end = start->start + start->length;
        const auto distance = end->start - start_end;
        return (distance + 1) * (min_edge_cost_ever + min_token_cost_ever);
    }
    
    float calculate_cost(const bettertoken * start, const bettertoken * end)
    {
        auto start_end = start->start + start->length;
        if(start_end != end->start)
            return FLT_MAX;
        
        if(end->type != 0)
            return 0;
        
        int32_t specific_cost = end->cost;
        
        if(start->type != 0)
            return float(specific_cost);
        
        int32_t connection_cost = connection_matrix[start->right_context + left_edges*end->left_context];
        
        return float(specific_cost + connection_cost);
    }

    void AdjacentCost(void * state, MP_VECTOR<micropather::StateCost> * adjacent)
    {
        auto start = (bettertoken *)state;
        if(start->type == 0 || start->type == 3)
        {
            auto start_end = start->start + start->length;
            if(start_end == lattice.size())
            {
                adjacent->push_back({&dummy_ending, calculate_cost(start, &dummy_ending)});
            }
            else
            {
                for(auto & end : lattice[start_end])
                    adjacent->push_back({&end, calculate_cost(start, &end)});
            }
        }
        else if(start->type == 1)
        {
            for(auto & end : lattice[0])
                adjacent->push_back({&end, calculate_cost(start, &end)});
        }
        else
            return;
    }

    void PrintStateInfo(void * state)
    {
        (void)state;
    }
};

int main(int argc, char ** argv)
{
    if(argc < 4)
        return puts("usage: ./dedart sys.dic matrix.bin file_containing_text_to_parse.txt"), 0;
    
    auto f = fopen(argv[1], "rb");
    if(!f)
        return puts("failed to open sys.dic file"), 0;
    
    // magic
    auto magic = read_u32(f);
    if (magic != 0xE1172181)
        return puts("not a mecab sys.dic file"), 0;
    
    // 0x04
    auto version = read_u32(f);
    if (version != 0x66)
        return puts("unsupported version"), 0;
    
    World world;
    
    // 0x08
    auto type = read_u32(f); // 0 - sys; 1 - user; 2 - unk
    (void)type; // surpress unused var warnings
    auto num_tokens = read_u32(f); // number of unique recognizable lexemes / number of token information entries
    // 0x10
    auto num_left_contexts = read_u32(f);
    (void)num_left_contexts; // surpress unused var warnings
    auto num_right_contexts = read_u32(f);
    (void)num_right_contexts; // surpress unused var warnings
    
    // 0x18
    auto linkbytes = read_u32(f); // number of bytes used to store the dual-array trie
    auto tokenbytes = read_u32(f); // number of bytes used to store the list of tokens
    // 0x20
    auto featurebytes = read_u32(f); // number of bytes used to store the feature string pile
    read_u32(f); // padding
    // 0x28
    
    // 0x28
    // location of the encoding spec string
    char encoding[0x20];
    fread(encoding, 1, 0x20, f);
    
    if(strncmp(encoding, "UTF-8", 0x20) != 0)
        return puts("unsupported sys.dic encoding (only utf-8 is supported). stop using legacy encodings for infrastructure!");
    
    // 0x48
    // the dual-array trie starts here
    // it's a fucked up associative search tree that follows links that point to tables that can be compressed together by overlapping them
    // :|
    
    // we work out of memory, not mmapped files, because we're not barbarians and  ***20MB REALLY IS NOT A LOT OF MEMORY GUYS JESUS CHRIST***.
    link * link_array = (link *)malloc(linkbytes);
    uint32_t link_arraylen = linkbytes/sizeof(link);
    uint32_t read = fread(link_array, sizeof(link), link_arraylen, f);
    if(read != link_arraylen)
        return puts("error: dictionary is broken (couldn't find all the links we need)");
    if(linkbytes != link_arraylen*sizeof(link))
        return printf("error: inconsistent number of links (%u vs %u)", uint32_t(linkbytes/sizeof(link)), link_arraylen);
    
    // arbitrary offset
    // token array starts right after the dual-array trie link array
    
    token * token_array = (token *)malloc(tokenbytes);
    uint32_t token_arraylen = tokenbytes/sizeof(token);
    read = fread(token_array, sizeof(token), token_arraylen, f);
    if(read != token_arraylen)
        return puts("error: dictionary is broken (couldn't find all the tokens we need)");
    if(tokenbytes != token_arraylen*sizeof(token) or token_arraylen != num_tokens)
        return printf("error: inconsistent number of tokens (%u vs %u vs %u)", uint32_t(tokenbytes/sizeof(token)), token_arraylen, num_tokens);
    
    // arbitrary offset
    // pile of strings (contiguous null-terminated strings with no explicit location data) for "feature" data starts here
    // indexed by raw character offset from start of pile, not "which string is it"
    
    char * feature_strpile = (char *)malloc(featurebytes);
    read = fread(feature_strpile, 1, featurebytes, f);
    if(read != featurebytes)
        return puts("error: dictionary is broken (couldn't read out the entire feature string table)");
    
    fclose(f);
    
    // now we load the connection left-context to right-context cost matrix
    
    f = fopen(argv[2], "rb");
    if(!f)
        return puts("failed to open matrix.bin file"), 0;
    world.left_edges = read_u16(f);
    world.right_edges = read_u16(f);
    auto matrix_weights = world.left_edges*world.right_edges;
    
    world.connection_matrix = (int16_t *)malloc(matrix_weights*2);
    
    read = fread(world.connection_matrix, 2, matrix_weights, f);
    if(read != matrix_weights)
        return puts("error: connection matrix is broken (couldn't read out the entire weight matrix table)");
    
    fclose(f);
    
    for(size_t i = 0; i < matrix_weights; i++)
    {
        int16_t cost = world.connection_matrix[i];
        if(cost > max_edge_cost_ever)
            max_edge_cost_ever = cost;
        if(cost < min_edge_cost_ever)
            min_edge_cost_ever = cost;
    }
    
    // copy text to parse into a string
    
    f = fopen(argv[3], "rb");
    if(!f)
        return puts("failed to open parsing input file"), 0;
    
    auto string_to_parse = read_entire_file_into_string(f);
    
    fclose(f);
    
    // traverse the dual-array trie through brute force, searching for all possible valid links, recording the path (key) taken to each valid output link we find
    // sound crazy? it takes like 0.6ish seconds.
    
    std::vector<entry> collection;
    collect_down(link_array, link_arraylen, link_array[0].base, collection);
    
    // unravel everything into a dictionary.
    
    bool print_json = false;
    
    if(print_json)
    {
        printf("[\n");
        printf(" {");
    }
    
    bool is_first = true;
    for(const auto & finder : collection)
    {
        const auto & surface = finder.key;
        collect_prefixes(surface, world.contains_longer);
        if(print_json)
        {
            if(!is_first)
                printf(",");
            printf("\n  \"");
            print_str_json_escaped(surface);
            printf("\" :\n  [\n");
        }
        is_first = false;
        
        auto first = finder.value >> 8;
        auto count = finder.value & 0xFF;
        
        std::vector<bettertoken> tokens(count);
        for(uint32_t i = 0; i < count; i++)
        {
            tokens[i].assign(token_array[first+i], feature_strpile, surface);
            if(print_json)
            {
                printf("   [%u, %u, %u, %u, \"", tokens[i].left_context, tokens[i].right_context, tokens[i].pos, tokens[i].cost);
                print_str_json_escaped(tokens[i].feature);
                printf("\"]");
                if(i+1 < count)
                    printf(",");
                printf("\n");
            }
        }
        
        if(print_json)
            printf("  ]");
        
        world.dictionary[surface] = tokens;
    }
    
    if(print_json)
    {
        printf("\n },");
        printf("\n [");
        uint32_t i = 0;
        for(const auto & str : world.contains_longer)
        {
            printf("\n \"");
            print_str_json_escaped(str);
            printf("\"");
            if(i+1 < world.contains_longer.size())
                printf(",");
            i++;
        }
        printf("\n ]");
        printf("\n]");
    }
    
    auto indexes = build_indexes(string_to_parse.data());
    
    size_t max_covered_byte = 0;
    
    world.dummy_beginning = {0,0,0,0,"","BOS",0,0,1};
    
    // the last index contains one byte past the end of the string
    for(size_t start = 0; start+1 < indexes.size(); start++)
    {
        size_t end = start+1;
        
        auto start_byte = indexes[start];
        auto end_byte = indexes[end];
        auto len = end_byte - start_byte;
        
        auto substr = string_to_parse.substr(start_byte, len);
        
        std::vector<bettertoken> tokens_that_start_here;
        
        while(world.contains_longer.count(substr) == 1 or world.dictionary.count(substr) == 1)
        {
            if(world.dictionary.count(substr))
            {
                for(auto token : world.dictionary[substr])
                {
                    token.start = start;
                    tokens_that_start_here.push_back(token);
                }
                if(end_byte > max_covered_byte)
                    max_covered_byte = end_byte;
            }
            
            end += 1;
            if(end >= indexes.size())
                break;
            end_byte = indexes[end];
            len = end_byte - start_byte;
            
            substr = string_to_parse.substr(start_byte, len);
        }
        
        if(start_byte == max_covered_byte and tokens_that_start_here.size() == 0)
            tokens_that_start_here.push_back({0,0,0,0,substr,"UNK",start,end-start,3});
        
        world.lattice.push_back(tokens_that_start_here);
    }
    
    world.dummy_ending = {0,0,0,0,"","EOS",indexes.size()-1,0,2};
    
    micropather::MicroPather pather(&world);
    MP_VECTOR<void *> path_holder;
    float totalCost = 0;
    int result = pather.Solve(&world.dummy_beginning, &world.dummy_ending, &path_holder, &totalCost);
    printf("result: %d\n", result);
    
    std::vector<bettertoken *> path;
    for(auto ptr : path_holder)
    {
        if(ptr != (void *)&world.dummy_beginning and ptr != (void *)&world.dummy_ending)
            path.push_back((bettertoken *)ptr);
    }
        
    for(auto token : path)
        printf("%s\n", token->feature.data());
    for(auto token : path)
    {
        printf("%s", token->surface.data());
        if(token != path.back())
            printf("｜");
        else
            printf("\n");
    }
}