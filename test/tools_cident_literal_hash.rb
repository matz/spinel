# A literal holding the tree's path compares equal against its own commit:
# the reference's rename gives it the hash of its renamed bytes too.
puts `bash tools/cident_cache_test.sh literal`
