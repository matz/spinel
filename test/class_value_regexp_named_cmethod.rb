# A class method named as one of Regexp's readers, called on a class held
# in a block parameter, is the class's method.
class T
  def self.names = public_instance_methods(true).grep(/\Atest_/).map(&:to_s)
  def self.source = "t-source"
  def self.options = [:a, :b]
  def self.casefold? = true
  def self.named_captures = { "k" => 1 }
  def test_a = 1
end

p T.names
[T].each do |k|
  p k.names
  p k.source
  p k.options
  p k.casefold?
  p k.named_captures
end

# an inherited class method that returns the Symbols
class Base
  def self.names = public_instance_methods(true).grep(/\Atest_/)
end

class U < Base
  def test_b = 1
end

[U].each { |k| p k.names }

# a Regexp in a block parameter of the same program
[/(?<year>\d+)-(?<mon>\d+)/i].each do |re|
  p re.names
  p re.source
  p re.options
  p re.casefold?
  p re.named_captures
end

# a class and a Regexp in one Array
[T, /(?<day>\d+)/].each do |v|
  p v.names
  p v.source
  p v.options
  p v.named_captures
end

# a parameter that holds a Regexp or a class with the class methods
class R
  def self.casefold? = true
end

def cf(k) = k.casefold?
p cf(/a/i)
p cf(/a/)
p cf(R)

class W
  def self.casefold? = 7
  def self.source = :sym
  def self.options = "opts"
  def self.names = [1, 2]
  def self.named_captures = nil
end

def w_cf(k) = k.casefold?
def w_so(k) = k.source
def w_op(k) = k.options
def w_nm(k) = k.names
def w_nc(k) = k.named_captures

[/(?<y>a)/i, W].each do |v|
  p w_cf(v)
  p w_so(v)
  p w_op(v)
  p w_nm(v)
  p w_nc(v)
end
