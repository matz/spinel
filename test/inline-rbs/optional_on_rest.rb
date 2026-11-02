# Signatures that declare an optional positional where the def gathers its
# arguments with a rest, as a Hash-like `fetch(key, *default)` writes it
# under `(Symbol, ?untyped)`. The optional describes what the rest gathers,
# which stays the gathered array as any rest does; the other parameters and
# the return take their types, and the program compiled with
# `--no-inline-rbs --rbs sig/optional_on_rest` produces the same C byte for
# byte. Each annotation pins a slot to `untyped`, which inference would not
# choose on its own, so the C shows whether it applied.
class Bag
  def initialize = @h = { 1 => "one" }

  #: (untyped, ?untyped) -> untyped
  def fetch(key, *default)
    return @h[key] if @h.key?(key)
    return default.first unless default.empty?
    raise KeyError, "key not found: #{key.inspect}"
  end

  #: (untyped, ?untyped, ?strict: untyped) -> untyped
  def get(key, *default, strict: false)
    strict ? fetch(key) : fetch(key, *default)
  end
end

b = Bag.new
p b.fetch(1), b.fetch(2, "none")
p b.get(1), b.get(2, "none"), b.get(1, strict: true)
