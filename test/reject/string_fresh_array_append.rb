# A fresh Array cannot lend its String element to an appending block.
p([+"a"].each { |x| x << "!" })
