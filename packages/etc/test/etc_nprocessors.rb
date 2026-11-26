# Etc.nprocessors returns the number of CPUs this process may use
require "etc"

n = Etc.nprocessors
p n.is_a?(Integer)
p n.positive?
p n == Etc.nprocessors
