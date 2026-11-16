# A writer's name sent with keywords alone (`**h`) to a union of a class with
# `def x=` and one with an attr_accessor: the writer takes their Hash, as CRuby
# hands it to a method without keywords
class Ctl
  def request=(r)
    @request = r
  end
end
class Conn
  attr_accessor :request
end
o = [Ctl.new, Conn.new][(ARGV[1] || 1).to_i]
h = { c: 3 }
o.__send__((ARGV[0] || "request=").to_sym, **h)
p o.instance_variable_get(:@request)
