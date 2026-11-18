# A computed-name send of a writer (`public_send("#{name}=", value)`) on a
# receiver of no known class dispatches over every class: an `attr_accessor`
# in a subclass of a class with an explicit `def logger=` is that subclass's
# writer, and the dispatch has one arm for it, not the inherited def's as well.
class RailtieConf
  def logger = (raise(NotImplementedError, "pruned"))
  def logger=(v); raise(NotImplementedError, "pruned") end
end
class EngineConf < RailtieConf
  attr_writer :eager_load_paths
end
class AppConf < EngineConf
  attr_accessor :logger
end
class Deprecators
  def initialize; @options = {}; @deprecators = []; end
  def logger=(v); set_option(:logger, v); end
  def add(d); @deprecators << d; end
  def set_option(name, value); @options[name] = value; @deprecators.each { |d| apply_options(d) }; end
  private
  def apply_options(deprecator)
    @options.each do |name, value|
      deprecator.public_send("#{name}=", value)
    end
  end
end
d = Deprecators.new
app = AppConf.new
d.add(app)
d.logger = ARGV.empty? ? 7 : "seven"
p app.logger
p app.frozen?
