# A keyword alias of respond_to? asks of any method by name.
class T
  def secret = "s"
  alias rt respond_to?
  def last_one = :kept
end
p T.new.rt(:secret)
