# A Method of a module's class method it does not have raises NameError,
# which names '#<Class:V>' once the module's singleton class is made and
# 'Module' before. Here a singleton_class call that may or may not run
# makes it, so the compile cannot state the message and refuses the
# capture, saying why (cplan_refuse_resolve).
module V
  def b = 1
end
cond = ARGV.empty?
V.singleton_class if cond
begin
  V.method(:b)
rescue NameError => e
  p e.message
end
