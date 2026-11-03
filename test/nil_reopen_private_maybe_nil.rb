# A private method the program gives NilClass is no answer for a call with
# an explicit receiver that turns out nil: NoMethodError, as CRuby raises
# for `nil.secret` written out. The nil arm of a call whose receiver may be
# nil (a typed pointer, a boxed value) checks the visibility as the
# literal nil does, and a public one still answers there, also where the
# receiver's own class has no method of the name (the other arm raises).
class NilClass
  private def secret = :s
  def open = secret
  def shown = :shown
end
class K
  def secret = :k
  def shown = :kshown
end
def pick(f) = f ? K.new : nil
x = pick(ARGV.size > 5)
p((x.secret rescue :nme))
p x.open
p x.shown
y = nil
p((y.secret rescue :nme))
p y.open
p y.shown
p((nil.secret rescue :nme))
p nil.open
k = pick(true)
p k.secret, k.shown
p((k.open rescue :nme))
