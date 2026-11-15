# A public top-level def of a name a class of the program answers with an
# attribute (a reader, a writer, a Struct or Data member) does not take the
# call where the receiver may be a value of that class: the class's own
# accessor answers, as the class's method would.
def token = :top
public :token
class A
  attr_reader :token
  def initialize = @token = :a_tok
end
class R
  attr_reader :token
  def initialize = @token = :r_tok
end
[A.new, R.new, A.new].each { |o| p o.token }

def name2 = :top2
public :name2
S = Struct.new(:name2)
D = Data.define(:name2)
[S.new(1), D.new(2)].each { |o| p o.name2 }

def val=(v)
  :top
end
public :val=
class W
  attr_writer :val
  def get = @val
end
class W2
  attr_accessor :val
  def get = @val
end
[W.new, W2.new].each { |o| o.val = 4; p o.get }

# a class's or module's singleton accessor answers ahead of the public def,
# called on the class itself
def tk = :top
public :tk
class Z
  @tk = 5
  class << self; attr_reader :tk; end
end
module ZM
  @tk = 6
  class << self; attr_accessor :tk; end
end
p Z.tk, ZM.tk
def w=(v)
  :top_w
end
public :w=
class Z2
  class << self; attr_accessor :w; end
end
Z2.w = 3
p Z2.w

# another class's accessor, or only a writer, leaves a class value the def
def tk2 = :top2
public :tk2
class Za; @tk2 = 5; class << self; attr_reader :tk2; end; end
class Zb; end
class Zw; class << self; attr_writer :tk2; end; end
Zw.tk2 = 4
p Za.tk2, Zb.tk2, Zw.tk2, String.tk2
k = ARGV.empty? ? String : Zb
p k.tk2
