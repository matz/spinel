# A table-backed character, a substring, and a substitution each return
# a String that can be retained in a mixed Array and grown through an alias.
def grow_first(row)
  value = row[0]
  value << "x" * 96
  p [value.bytesize, row[0].bytesize, value.frozen?]
end

number = [65, "unused"][0]
grow_first([number.chr(Encoding::UTF_8), 1])
p number.chr(Encoding::UTF_8)

source = [+"a\0bc", 7][0]
grow_first([source.delete_prefix("no match"), 2])
grow_first([source.delete_suffix("c"), 3])
grow_first([source.sub(/no match/, "z"), 4])
grow_first([source.gsub(/b/, "z"), 5])
p source.bytes
