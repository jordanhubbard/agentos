/* Normalize only the source-path comment emitted by seL4's bitfield generator.
 * The generated declarations and the installed input SDK remain unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char prefix[] = "/* generated from ";
    char buffer[8192];
    if (argc != 3) {
        fprintf(stderr, "usage: normalize-header INPUT FRESH_OUTPUT\n");
        return 1;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input) { perror(argv[1]); return 1; }
    if (!fgets(buffer, sizeof buffer, input) ||
        strncmp(buffer, prefix, sizeof prefix - 1) != 0) {
        fprintf(stderr, "missing generated-source comment: %s\n", argv[1]);
        fclose(input);
        return 1;
    }
    char *source = buffer + sizeof prefix - 1;
    char *relative = strstr(source, "sel4/libsel4/");
    size_t length = strlen(buffer);
    if (!relative || (relative != source && relative[-1] != '/') ||
        length < 4 || strcmp(buffer + length - 4, " */\n") != 0) {
        fprintf(stderr, "unexpected generated-source comment: %s\n", argv[1]);
        fclose(input);
        return 1;
    }
    FILE *output = fopen(argv[2], "wbx");
    if (!output) { perror(argv[2]); fclose(input); return 1; }
    int failed = fputs(prefix, output) == EOF || fputs(relative, output) == EOF;
    size_t count;
    while (!failed && (count = fread(buffer, 1, sizeof buffer, input)) != 0)
        failed = fwrite(buffer, 1, count, output) != count;
    failed |= ferror(input) != 0;
    failed |= fclose(input) != 0;
    failed |= fclose(output) != 0;
    if (failed) {
        fprintf(stderr, "failed to normalize %s\n", argv[1]);
        remove(argv[2]);
    }
    return failed ? 1 : 0;
}
