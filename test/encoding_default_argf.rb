# ARGF and STDIN reads follow Encoding.default_external.
p gets.encoding
Encoding.default_external = Encoding::ASCII_8BIT
p gets.encoding
p $stdin.gets.encoding
p readline.encoding
p ARGF.gets.encoding
s = ARGF.read
p s, s.encoding
Encoding.default_external = "utf-8"
p ARGF.read.encoding
