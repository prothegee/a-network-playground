clang-format --version;
find . -type f \
    \( -name "*.hh" -o -name "*.cc" \) \
    -not -name "*.in.hh" \
    -exec clang-format -style=file -i {} +    ;
echo "finished: formating .hh & .cc files"

