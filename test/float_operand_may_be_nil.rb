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

# The receiver comes first: it runs, and its own nil is what raises.
def both(a, z)
  $a = a
  $z = z
end

def recv
  puts "recv"
  $a
end

def int_recv
  puts "int_recv"
  7
end

def try_class
  p yield
rescue TypeError, NoMethodError => e
  puts e.class
end

both(2.0, nil)
try_class { recv ** $z }
try_class { recv % $z }
try_class { recv.divmod($z) }
try_class { recv.coerce($z) }
try_class { int_recv ** $z }
try_class { int_recv % $z }

both(nil, nil)
try_class { recv ** $z }
try_class { recv % $z }
try_class { recv.divmod($z) }
try_class { recv.coerce($z) }

# Both run before either's nil raises. An operand that raises comes first too.
def opnd
  puts "opnd"
  raise ArgumentError, "opnd" if $opnd_raises
  $z
end

def float_opnd
  puts "float_opnd"
  3.0
end

def local_pow(x)
  p(x ** opnd)
rescue TypeError, NoMethodError, ArgumentError => e
  puts e.class
end

def local_mod(x)
  p(x % opnd)
rescue TypeError, NoMethodError, ArgumentError => e
  puts e.class
end

def local_divmod(x)
  p(x.divmod(opnd))
rescue TypeError, NoMethodError, ArgumentError => e
  puts e.class
end

def local_coerce(x)
  p(x.coerce(opnd))
rescue TypeError, NoMethodError, ArgumentError => e
  puts e.class
end

def local_pow_float(x)
  p(x ** float_opnd)
rescue NoMethodError => e
  puts e.class
end

def local_mod_float(x)
  p(x % float_opnd)
rescue NoMethodError => e
  puts e.class
end

def local_divmod_float(x)
  p(x.divmod(float_opnd))
rescue NoMethodError => e
  puts e.class
end

def local_all(x)
  local_pow(x)
  local_mod(x)
  local_divmod(x)
  local_coerce(x)
end

$opnd_raises = false
$z = 3.0
local_all(2.0)
$z = nil
local_all(2.0)
local_all(nil)
$z = 3.0
local_all(nil)
$opnd_raises = true
local_all(nil)
local_pow_float(2.0)
local_mod_float(2.0)
local_divmod_float(2.0)
local_pow_float(nil)
local_mod_float(nil)
local_divmod_float(nil)
