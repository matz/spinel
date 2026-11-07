# Class#method(:sym) of a class method that no def, alias or
# define_method in the program makes raises NameError, as CRuby does.
# A builtin class method (new, members) and one the program defines
# still make a Method.
class P; def self.k = 1; def i = 2; end
class Q < P; end
S = Struct.new(:a)
D = Data.define(:a)
C = Class.new
p((Q.method(:zz) rescue $!.message))
p((P.method(:zz) rescue $!.message))
p((P.public_method(:zz) rescue $!.message))
p((S.method(:zz) rescue $!.message))
p((D.method(:zz) rescue $!.message))
p((C.method(:zz) rescue $!.message))
p((P.method(:zz) rescue $!.class))
p P.respond_to?(:zz)
p Q.method(:k).call, S.method(:members).name
p((P.method(:i) rescue $!.message))
p((P.new.method(:zz) rescue $!.message))
