# A top-level `undef` of a name a module the top level includes gives stops the lookup: a bare call after it raises
# NameError, and so does `method(:name)`.
module L
  def shout = :l
end
include L
p shout
p method(:shout).call
undef shout
begin
  shout
rescue NameError => e
  puts e.class
end
begin
  method(:shout)
rescue NameError => e
  puts e.class
end
