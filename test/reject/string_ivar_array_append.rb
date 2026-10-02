# An ivar Array cannot lend its String element to an appending block.
s = +"a"
@a = [s]
@a.each { |x| x << "!" }
p s
