# A name a module defines for both Integer and nil: a typed Array's index
# answer that is nil calls the module's method as nil does.
module L
  def label = "L #{inspect}"
end
class Integer
  include L
end
class NilClass
  include L
end
ia = [3, 1, 4]
miss = ARGV.size == 0 ? 99 : 4
p ia.index(miss).label, ia.index(1).label
