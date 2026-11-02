# Signatures over methods that forward their arguments with `...`, as a
# delegator writes them (`def self.parse(...) = Backend.parse(...)`). The
# forwarded arguments go on to another method, which types them: the
# signature's return type applies, each named positional ahead of the `...`
# takes the signature's positional in its place, and the program compiled
# with `--no-inline-rbs --rbs sig/forwarding_params` produces the same C byte
# for byte. Each annotation pins a slot to `untyped`, which inference would
# not choose on its own, so the C shows whether it applied.
class Backend
  def self.parse(src, strict: false) = strict ? src.upcase : src.size
  def self.join(a, b, sep: "-") = "#{a}#{sep}#{b}"
  def self.pair(x, y = 0) = x + y
end

class Front
  #: (String, ?strict: bool) -> untyped
  def self.parse(...) = Backend.parse(...)

  #: (untyped, String, ?sep: String) -> untyped
  def self.join(a, ...) = Backend.join(a, ...)

  #: (Integer, ?untyped, ?Integer) -> untyped
  def self.pair(n, w = 1, ...) = Backend.pair(n * w, ...)
end

p Front.parse("ab"), Front.parse("ab", strict: true)
p Front.join(1, "x"), Front.join(2, "y", sep: "+")
p Front.pair(3), Front.pair(3, 2), Front.pair(3, 2, 5)
