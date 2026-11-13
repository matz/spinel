# A class's class-side method list in a method that runs before and after a
# def of a class method, or while a method that makes the def runs twice, is
# answered by the version cells the def's statement sets, as a call or a
# respond_to? there is.

class Before; end
def show_before = p(Before.singleton_methods(false))
show_before
def Before.v = 1
show_before

class Twice; end
def make_twice = (def Twice.v = 1)
def show_twice = p(Twice.singleton_methods(false))
show_twice
make_twice
show_twice
make_twice
show_twice

class Guard; end
def guard = Guard.singleton_methods(false).include?(:v) ? Guard.v : :none
p guard
def Guard.v = 7
p guard

class Inh; end
class InhSub < Inh; end
def show_inh = p(InhSub.singleton_methods)
show_inh
def Inh.w = 1
show_inh

class Inc; end
def has_v = Inc.singleton_methods(false).include?(:v)
a = has_v
def Inc.v = 1
b = has_v
p [a, b]

class Own; def self.a = 1; end
class OwnSub < Own; end
def show_own = p(OwnSub.singleton_methods(false))
show_own
def OwnSub.s = 2
show_own
