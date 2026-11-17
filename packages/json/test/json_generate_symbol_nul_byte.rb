require "json"
# A Symbol's name may hold a NUL byte, as a String does: JSON.generate writes
# each as \u0000 and goes on to the name's end, as CRuby's json does, rather
# than stopping at the first one.

puts JSON.generate([:"s\0t"])
puts JSON.generate({:"k\0y" => 1})
puts :"a\0b".to_json
puts JSON.generate({"x" => :"v\0w"})
puts JSON.generate([:plain, :"é", :"", :"\0", :"a\"b\n"])
puts JSON.pretty_generate({:"p\0q" => [:"r\0s"]})
puts JSON.dump([:"d\0e"])

# a Symbol made at run time, as a value and as a key
s = "m" + "\0" + "n"
puts JSON.generate([s.to_sym])
puts JSON.generate({s.to_sym => s.to_sym})

# a boxed Symbol, and a key beside a plain one
x = [:"bx\0y", 1][0]
puts x.to_json
puts({a: 1, "b\0c": 2}.to_json)

# it round-trips
puts JSON.parse(JSON.generate([:"z\0z"]))[0] == "z\0z"
