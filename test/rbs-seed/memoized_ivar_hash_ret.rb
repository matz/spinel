# A class-level ivar memoizing an empty hash, read through a signature that
# pins the reader's return to Hash[String, String].
#
# The ivar never narrows: its only assignment is `{}`, and every write that
# would give it element types goes through the value the reader hands back. So
# the body is poly while the signature says string-valued, and the two do not
# share a layout -- the same mismatch `hash_kind_widened_return` covers from
# the other side, where the BODY was the narrower kind.
#
# Emitting the poly body through a StrStrHash* signature reinterprets the
# pointer: the `||=` spelling crashed on the first read, and the equivalent
# nil-guard spelling did not compile at all (an sp_RbVal returned into a
# `const char *`). The declared type made the program worse than leaving it
# undeclared, which it must not.
module Registry
  def self.table
    @table ||= {}
  end

  # the same memo, spelled without `||=`
  def self.other
    @other = {} if @other.nil?
    @other
  end
end

Registry.table["a"] = "1"
Registry.table["b"] = "2"
p Registry.table["a"]
p Registry.table.size

Registry.other["k"] = "v"
p Registry.other["k"]

acc = []
Registry.table.each { |k, v| acc << "#{k}=#{v}" }
p acc
p Registry.table.keys
