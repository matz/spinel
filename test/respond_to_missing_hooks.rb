# respond_to? consults respond_to_missing?: a program defining only that hook
# still has Object's default behind it, and a class object asks its own
# singleton hook.
class Only
  def respond_to_missing?(name, all = false) = name == :virt
end
o = Only.new
p o.respond_to?(:virt), o.respond_to?(:other), 5.respond_to?(:virt)
class K
  def self.method_missing(name, *args) = name == :virtual ? :v : super
  def self.respond_to_missing?(name, all = false) = name == :virtual
end
p K.respond_to?(:virtual), K.respond_to?(:nope), K.virtual
