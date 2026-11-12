# An alias of a Hash subclass's builtin method, ahead of the class's own method of the
# name, is a method a subclass overrides and reaches with super (Hashie::Mash's
# alias regular_writer []=, OmniAuth::AuthHash's regular_writer)
class Base < Hash
  alias_method :regular_writer, :[]=
  def []=(key, value)
    regular_writer(key.to_s, value)
  end
end
class Auth < Base
  def regular_writer(key, value)
    value = value.to_s.upcase if key.to_s == "info"
    super
  end
end
a = Auth.new
a[:provider] = "developer"
a[:info] = "x"
p a
class Plain < Base
end
pl = Plain.new
pl[:k] = 1
p pl
