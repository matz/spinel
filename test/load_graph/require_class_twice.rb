def setup = require_relative("require_class_twice_lib")
def twice = yield + yield
setup
p twice { Lib.v }
