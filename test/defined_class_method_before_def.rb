# defined? of a class method before the program's def of it is nil, as
# CRuby has it, and "method" after it: on a plain class, a Struct class
# and a Data class.

class C; end
p defined?(C.late)
def C.late = 1
p defined?(C.late)

S = Struct.new(:a)
p defined?(S.late)
def S.late = 2
p defined?(S.late)

D = Data.define(:x)
p defined?(D.late)
def D.late = 3
p defined?(D.late)

# a builtin's class method is defined from the start
p defined?(S.members), defined?(C.name)
