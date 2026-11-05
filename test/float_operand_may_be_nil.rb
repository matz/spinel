# A Float operand that may be nil, like a global or an instance variable set
# only inside a method, reached `**`, `%`, `divmod` and `coerce` as a refusal at
# compile time, where `+` takes it. A number works, and nil is the TypeError.
def set(v)
  $z = v
end

class Box
  def fill
    @z = 1.5
    self
  end

  def clear
    @z = nil
    self
  end

  def float_pow = 2.0 ** @z
  def float_mod = 7.5 % @z
  def int_pow = 2 ** @z
  def int_mod = 7 % @z
  def bignum_pow = (2**70) ** @z
  def bignum_mod = (2**70) % @z
  def float_divmod = 7.5.divmod(@z)
  def float_coerce = 7.5.coerce(@z)
end

def globals
  [2.0 ** $z, 7.5 % $z, 2 ** $z, 7 % $z, 7.5.divmod($z), 7.5.coerce($z)]
end

def try
  p yield
rescue TypeError => e
  puts "TypeError: #{e.message}"
end

set(3.0)
p globals

box = Box.new.fill
p box.float_pow, box.float_mod, box.int_pow, box.int_mod
p box.bignum_pow, box.bignum_mod
p box.float_divmod, box.float_coerce

set(nil)
try { 2.0 ** $z }
try { 7.5 % $z }
try { 2 ** $z }
try { 7 % $z }
try { 7.5.divmod($z) }
try { 7.5.coerce($z) }

box.clear
try { box.float_pow }
try { box.float_mod }
try { box.int_pow }
try { box.int_mod }
try { box.bignum_pow }
try { box.bignum_mod }
try { box.float_divmod }
try { box.float_coerce }
