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
# builtin class methods of any visibility still make a Method; a Method
# read for its name raises the same NameError
p(((P.method(:attr_accessor); :ok) rescue $!.message))
p(((P.method(:private_class_method); :ok) rescue $!.message))
p(((P.method(:included); :ok) rescue $!.message))
p(((P.method(:instance_variable_names); :ok) rescue $!.message))
p((P.method(:zz).name rescue $!.message))
# a name that needs escaping in C
p((P.method(:"a\"b\\c") rescue $!.message))
p((P.method(:"x\ny%sz") rescue $!.message))
Q9 = Class.new { def self.k = 1 }
p((Q9.method(:"q\"%d") rescue $!.message))
