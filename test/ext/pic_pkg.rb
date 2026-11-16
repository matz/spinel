# A library that requires a bundled package with C of its own (json), built
# into a shared object: its C names packages/json/sp_json.o
# (SPINEL_LINK_OBJ), and ext-test links that object's position-independent
# build, sp_json_pic.o, in its place. The package object was built for an
# executable only, and `cc -shared` refused its relocations.
# test/ext/pic_pkg_host.c loads the object with dlopen.
require "json"

module ExtPicPkg
  #: (Integer) -> String
  def self.dump(n) = JSON.generate({ "n" => n, "list" => [1, 2, 3] })

  #: (Integer) -> Integer
  def self.sum(n) = JSON.parse(dump(n))["list"].sum + n
end
