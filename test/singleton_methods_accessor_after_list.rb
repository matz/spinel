# A class's singleton_methods answers as of when it runs. A class-side
# accessor made after the list is not in it, at the top level and in a
# block that runs at once; a def made between two lists in such a block
# is in the second alone.

class C3; end
p C3.singleton_methods
class << C3; attr_accessor :x; end
p C3.singleton_methods.sort

class C4; end
a = [1]
a.each { p C4.singleton_methods.sort }
class << C4; attr_reader :y; end
a.each { p C4.singleton_methods.sort }

class C5; end
a.each { p C5.singleton_methods }
def C5.z = 1
a.each { p C5.singleton_methods }
