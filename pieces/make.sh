gcc -o editor -Ipiece_buf -I tree-sitter/lib/include/tree-sitter -Itree-sitter/lib/include piece_buf/piece_table.c editor.c -lncurses -L tree-sitter -ltree-sitter -lm -ldl
