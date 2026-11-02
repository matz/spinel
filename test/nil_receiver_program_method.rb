# A receiver that may be nil, calling a method its class has and nil has
# too, from the program: nil runs NilClass's (or Object's), as CRuby does,
# and any other value its own class's. Before, the class's method ran with
# nil as self (`x.blank?` raised NoMethodError for `empty?` on nil).

class Object
  def blank?
    respond_to?(:empty?) ? !!empty? : false
  end

  def shown
    "obj:#{self}"
  end
end

class NilClass
  def blank?
    true
  end

  def label
    "none"
  end

  def note(log)
    log << "nil"
  end

  def maybe
    nil
  end
end

class String
  def blank?
    strip.empty?
  end

  def label
    upcase
  end

  def shown
    "str:#{self}"
  end

  def note(log)
    log << self
  end

  def maybe
    self
  end
end

class Array
  def label
    "array of #{size}"
  end
end

class Hash
  def label
    "hash of #{size}"
  end
end

class Box
  def initialize(v)
    @v = v
  end

  def label
    "box #{@v}"
  end
end

def str_or_nil(f) = f ? " a " : nil
def arr_or_nil(f) = f ? [1, 2] : nil
def hash_or_nil(f) = f ? {a: 1} : nil
def box_or_nil(f) = f ? Box.new(3) : nil

[false, true].each do |f|
  s = str_or_nil(f)
  p s.blank?, s.label, s.shown, s.maybe
  p str_or_nil(f).label
  p arr_or_nil(f).label, hash_or_nil(f).label, box_or_nil(f).label
  log = []
  s.note(log)
  p log
end

# an argument runs once, ahead of the receiver's test
calls = 0
counted = -> { calls += 1; [] }
s = str_or_nil(false)
p s.note(counted.call), calls
