# Signatures over methods with optional, rest and trailing positionals and a
# `**` rest, as Active Support writes them (`in_groups(number, fill_with =
# nil, &block)`), in the `#:` form and as `# @rbs` parameter lines. Each
# positional takes the signature's type of its kind in order, the rest and
# `**` slots stay the gathered array and hash, and the program compiled with
# `--no-inline-rbs --rbs sig/optional_params` produces the same C byte for
# byte. Each annotation pins a slot to `untyped`, which inference would not
# choose on its own, so the C shows whether it applied.
class Fmt
  #: (untyped, ?untyped) -> untyped
  def pad(s, w = 4) = s.to_s.rjust(w)

  #: (untyped, *untyped, untyped) -> untyped
  def around(a, *mid, z) = "#{a}#{mid.join}#{z}"

  #: (?untyped, ?untyped, k: untyped, **untyped) -> untyped
  def opts(x = 1, y = 2, k:, **rest) = [x, y, k, rest.size]

  # @rbs a: untyped
  # @rbs b: untyped
  # @rbs z: untyped
  # @rbs return: untyped
  def named(a, b = 2, *mid, z) = [a, b, mid.size, z]
end

f = Fmt.new
p f.pad(7), f.pad("ab", 3)
p f.around(1, 2), f.around(1, 2, 3, 4)
p f.opts(k: 0), f.opts(5, k: 0, z: 1), f.opts(5, 6, k: 0)
p f.named(1, 9), f.named(1, 2, 3, 4)
