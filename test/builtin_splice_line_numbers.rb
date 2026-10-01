p Gem::Version.new("1.2") < Gem::Version.new("1.10")
p __LINE__
p RbConfig::CONFIG.key?("host_os")
p __LINE__
o = Object.new
ObjectSpace.define_finalizer(o, proc { })
def where = __LINE__
p where
p __LINE__
