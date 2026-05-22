gcc -o editor -Ipiece_buf -I ts/tree-sitter/lib/include/tree-sitter -Its/tree-sitter/lib/include piece_buf/piece_table.c editor.c -lncurses -L libs -ltree-sitter -lm -ldl
