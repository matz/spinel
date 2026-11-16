# A boxed value that may be a Class, sent a name no class method of the
# program and none of Module's answer: the class arm declines, and the
# dispatch raises NoMethodError as CRuby does. The arm took the Hash
# emitter's answer for `empty?` and `none?` on the class value (its
# conversion of a Class is NULL, "empty"), and answered true.
class Req
  def initialize(h) = @h = h
  def params = @h
  def none? = @h.none?
end
def probe(request, method) = request.send(method)
[Req.new({ a: 1 }), { b: 2 }, {}, String, Class.new].each do |r|
  %i[params none? empty? name].each do |m|
    begin
      p probe(r, m)
    rescue NoMethodError => e
      puts "NoMethodError #{m}"
    end
  end
end
