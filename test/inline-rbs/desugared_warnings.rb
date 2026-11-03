# Annotations on defs an earlier pass moves, as Active Support writes them:
#
# - Enumerable's own methods run as copies, one per call site, each
#   carrying the def's annotation. One comment gets one verdict, under the
#   name the comment's reader wrote (`compact_blank`, not a copy's name),
#   and an annotation the copies apply (`exclude?`) is not reported as lost
#   with the def they were copied from. (`T` is no variable of Enumerable's.)
# - A TrueClass or FalseClass method Object also defines is a method of its
#   own class (#10859), so its signature applies and no warning is given.
module Enumerable
  #: () -> Array[T]
  def compact_blank
    reject { |x| x.nil? || x == "" }
  end

  #: (BasicObject) -> bool
  def exclude?(object)
    !include?(object)
  end
end

class Object
  #: () -> bool
  def blank? = respond_to?(:empty?) ? !!empty? : false
end

class FalseClass
  #: () -> true
  def blank? = true
end

class TrueClass
  #: () -> false
  def blank? = false
end

p [1, nil, "", 2].compact_blank, [3, nil].compact_blank, [1, 2].exclude?(3)
p [1, nil, false, true, ""].map(&:blank?)
