# A Symbol with an ASCII name is US-ASCII in CRuby, and inspect writes a
# control byte in it as \xNN. A Symbol with a non-ASCII character is UTF-8
# and writes the byte as \uNNNN, the same as a String.

p :"a\0b", :"\x01", "a\0b".to_sym
p :"a\0b".inspect
p :"a\e\x7f\t\x1f"
p :"é\0", "é\0".to_sym, :"é\x01\x7f"
p [:"\x02", :"a b"]
p({ :"a\0" => 1, :"é\0" => 2, :"\x01" => 3 })
p :"a\0"..:"a\x01"
p "a\0b"
