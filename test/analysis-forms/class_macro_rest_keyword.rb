# A class-body macro of a `class Module` reopening that takes a rest and a
# keyword (Active Support's `def delegate(*methods, to:)` shape) runs at its
# call: the rest is the names the call lists, an Array the macro indexes and
# sizes, and the String module_eval it builds defines them.
class Module
  def forward(*names, to:)
    module_eval("def #{names[0]} = #{to}.#{names[0]}")
    module_eval("def #{names[1]} = #{to}.#{names[1]}") if names.size > 1
  end

  def flag(name)
    module_eval("def #{name}? = true")
  end
end

class Config
  def host = "example.com"
  def port = 8080
end

class Server
  def config = Config.new
  forward :host, :port, to: :config
  flag :ready
end

# (the call is the body's last statement, and the macro's last line does
# not run: the class statement's value, nil, is read by no one)
class Mirror
  def config = Config.new
  forward :port, to: :config
end

s = Server.new
p s.host, s.port, s.ready?, Mirror.new.port
