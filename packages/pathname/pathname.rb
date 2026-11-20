# Spinel bundled `pathname`.
#
# A Pathname is a String path with methods on it, so a path stops being an
# untyped string that any other string can be mistaken for. It carries no
# state beyond that string: two Pathnames with equal strings are equal, hash
# alike, and sort by string order.
#
# The split is the same as CRuby's. The pure-path half (#+, #parent,
# #cleanpath, #relative_path_from, #descend) is string manipulation and never
# touches the disk, so it answers for paths that do not exist. The rest
# delegates to File and Dir with the receiver as the path argument.
#
# This covers the surface a program actually reaches for rather than every
# CRuby corner. Not modelled: the Find/FileUtils-backed corners (#find is here
# but #each_entry, #chown, #lchmod are not), #opendir, #sysopen, #make_link /
# #make_symlink and #birthtime. Kernel#Pathname() is the toplevel method at
# the end of this file, spelled the way BigDecimal() and URI() are.

class Pathname
  include Comparable

  SEPARATOR = "/"

  def initialize(path)
    # File.path checks #to_path, then #to_str.
    # Keep a copy so changing the caller's String cannot change the path.
    @path = File.path(path).dup
    raise ArgumentError, "path name contains null byte" if @path.include?("\0")
  rescue TypeError => e
    raise e.class, "Pathname.new requires a String, #to_path or #to_str", cause: nil
  end

  # The working directory, as a Pathname.
  def self.pwd
    new(Dir.pwd)
  end

  def self.getwd
    new(Dir.pwd)
  end

  # Every path matching the glob pattern, as Pathnames. A Float flags value
  # is truncated, as CRuby truncates it; a flags value that is neither an
  # Integer nor a Float raises TypeError (a Rational or an object with
  # #to_int is not accepted). Of the Integers, 0 takes the plain glob and
  # File::FNM_DOTMATCH the one that keeps hidden entries; any other value
  # raises NotImplementedError rather than reaching a Dir.glob that would not
  # apply it.
  def self.glob(pattern, flags = 0)
    flags = flags.to_i if flags.is_a?(Float)
    unless flags.is_a?(Integer)
      raise TypeError, "no implicit conversion from nil to integer" if flags.nil?
      what = flags == true || flags == false ? flags.to_s : flags.class.to_s
      raise TypeError, "no implicit conversion of #{what} into Integer"
    end
    paths = if flags == 0
      Dir.glob("#{pattern}")
    elsif flags == File::FNM_DOTMATCH
      Dir.glob("#{pattern}", File::FNM_DOTMATCH)
    else
      raise NotImplementedError, "Pathname.glob flags other than 0 and File::FNM_DOTMATCH"
    end
    paths.map { |x| new(x) }
  end

  # ---- conversion and identity ----

  def to_s
    @path
  end

  # The name File and Dir want. Defining it is what lets a Pathname be handed
  # to an API expecting a path without an explicit #to_s at every call.
  def to_path
    @path
  end

  def inspect
    "#<Pathname:#{@path}>"
  end

  def ==(other)
    other.is_a?(Pathname) && "#{other}" == @path
  end

  def eql?(other)
    self == other
  end

  def hash
    @path.hash
  end

  # String order over the paths. CRuby answers nil for a non-Pathname; a
  # single Integer return is what lets Comparable and #sort work here.
  def <=>(other)
    @path <=> "#{other}"
  end

  def freeze
    @path.freeze
    super
  end

  # ---- pure path manipulation (no disk access) ----

  # self + other, as CRuby's Pathname#plus joins them. An absolute `other`
  # replaces the receiver. Otherwise other's leading "." components drop and
  # each leading ".." takes the receiver's last name off
  # (`Pathname.new("a/b") + ".."` is `a`), and the rest of other is joined as
  # written: `a + "b/./c"` is `a/b/./c`, not cleaned.
  def +(other)
    # Every branch answers a Pathname. A method that returns its argument on
    # one path and a fresh object on another has no single type here, and the
    # boxed result made the NEXT `/` in a chain compile as numeric division.
    Pathname.new(Pathname.plus_str(@path, "#{other}"))
  end

  def /(other)
    self + other
  end

  def join(*parts)
    parts.reduce(self) { |acc, part| acc + part }
  end

  def parent
    self + ".."
  end

  def dirname
    Pathname.new(File.dirname(@path))
  end

  def basename(suffix = nil)
    Pathname.new(suffix ? File.basename(@path, suffix) : File.basename(@path))
  end

  def extname
    File.extname(@path)
  end

  # Replace the extension (or append one when there is none).
  def sub_ext(ext)
    old = File.extname(@path)
    Pathname.new(old == "" ? "#{@path}#{ext}" : "#{@path[0, @path.length - old.length]}#{ext}")
  end

  def absolute?
    @path.start_with?(SEPARATOR)
  end

  def relative?
    !absolute?
  end

  def root?
    s = @path
    return false if s == ""

    s.each_char { |ch| return false if ch != SEPARATOR }
    true
  end

  # Remove `.` components, resolve `..` against the preceding component, and
  # collapse repeated separators. Purely lexical: a `..` after a symlink is
  # NOT what the filesystem would resolve, which is what #realpath is for.
  def cleanpath
    Pathname.new(Pathname.clean_str(@path))
  end

  def expand_path(base = nil)
    Pathname.new(base ? File.expand_path(@path, "#{base}") : File.expand_path(@path))
  end

  # [dirname, basename]
  def split
    [dirname, basename]
  end

  # Each name component in order, without separators. With a block it yields
  # them and answers nil, as CRuby does; without one it answers an Enumerator
  # over them. So do #descend and #ascend.
  def each_filename
    return to_enum(:each_filename) unless block_given?
    Pathname.split_str(@path).each { |x| yield x }
    nil
  end

  # Root first, receiver last.
  def descend
    return to_enum(:descend) unless block_given?
    descend_paths.each { |x| yield x }
    nil
  end

  # Receiver first, root last.
  def ascend
    return to_enum(:ascend) unless block_given?
    descend_paths.reverse.each { |x| yield x }
    nil
  end

  # The Pathnames #descend walks, root first.
  def descend_paths
    out = []
    parts = Pathname.split_str(@path)
    acc = absolute? ? SEPARATOR : ""
    out.push(Pathname.new(SEPARATOR)) if absolute?
    parts.each do |part|
      acc = acc == "" ? part : (acc == SEPARATOR ? "#{SEPARATOR}#{part}" : "#{acc}#{SEPARATOR}#{part}")
      out.push(Pathname.new(acc))
    end
    out
  end
  private :descend_paths

  # The path that reaches self when resolved against base. Lexical, so it can
  # answer for paths that do not exist; raises when the two share no anchor
  # (one absolute, one relative).
  def relative_path_from(base)
    b = "#{base}"
    raise ArgumentError, "different prefix" if absolute? != b.start_with?(SEPARATOR)

    mine = Pathname.split_str(Pathname.clean_str(@path))
    cleaned_base = Pathname.clean_str(b)
    theirs = Pathname.split_str(cleaned_base)
    i = 0
    i += 1 while i < mine.length && i < theirs.length && mine[i] == theirs[i]
    j = i
    while j < theirs.length
      raise ArgumentError, "base_directory has ..: #{cleaned_base.inspect}" if theirs[j] == ".."
      j += 1
    end
    out = []
    (theirs.length - i).times { out.push("..") }
    j = i
    while j < mine.length
      out.push(mine[j])
      j += 1
    end
    Pathname.new(out.empty? ? "." : out.join(SEPARATOR))
  end

  # ---- predicates and stat (disk access) ----

  def exist?
    File.exist?(@path)
  end

  def file?
    File.file?(@path)
  end

  def directory?
    File.directory?(@path)
  end

  def symlink?
    File.symlink?(@path)
  end

  def readable?
    File.readable?(@path)
  end

  def writable?
    File.writable?(@path)
  end

  def executable?
    File.executable?(@path)
  end

  def size
    File.size(@path)
  end

  def zero?
    File.zero?(@path)
  end

  def ftype
    File.ftype(@path)
  end

  def mtime
    File.mtime(@path)
  end

  def atime
    File.atime(@path)
  end

  def ctime
    File.ctime(@path)
  end

  def stat
    File.stat(@path)
  end

  def lstat
    File.lstat(@path)
  end

  def realpath
    Pathname.new(File.realpath(@path))
  end

  def realdirpath
    Pathname.new(File.realdirpath(@path))
  end

  # ---- reading and writing ----

  def read
    File.read(@path)
  end

  def binread
    File.binread(@path)
  end

  def write(content)
    File.write(@path, content)
  end

  def binwrite(content)
    File.binwrite(@path, content)
  end

  def readlines
    File.readlines(@path)
  end

  # With a block, answers nil (CRuby's Pathname#each_line is IO.foreach's);
  # without one, an Enumerator over the lines.
  def each_line
    return to_enum(:each_line) unless block_given?
    File.foreach(@path) { |line| yield line }
    nil
  end

  def open(mode = "r")
    File.open(@path, mode) { |io| yield io }
  end

  # ---- directory contents ----

  # Names only, including "." and "..", as Pathnames -- matching CRuby.
  def entries
    Dir.entries(@path).map { |x| Pathname.new(x) }
  end

  # The children without "." and "..": full paths by default, bare names
  # with `with_directory` false (CRuby's argument of the same name).
  def children(with_directory = true)
    names = Dir.children(@path).sort
    with_directory ? names.map { |x| self + x } : names.map { |x| Pathname.new(x) }
  end

  # With a block, answers the children; without one, an Enumerator over them.
  def each_child(with_directory = true)
    return to_enum(:each_child, with_directory) unless block_given?
    out = children(with_directory)
    out.each { |x| yield x }
    out
  end

  def empty?
    directory? ? Dir.empty?(@path) : File.zero?(@path)
  end

  # Glob rooted at this directory.
  # CRuby globs relative to the receiver and joins each match back on with
  # `+`, whose one collapse that matters here is a receiver of "." vanishing:
  # `Pathname.new(".").glob("a/*.rs")` answers "a/top.rs", not "./a/top.rs".
  # A trailing separator or "/." on the receiver folds the same way ("a/" and
  # "a/." both answer "a/top.rs"), and anything else -- "./a" included -- is
  # kept as written, since `+` keeps it too. Joining the pattern onto the
  # folded receiver gives exactly those strings without going through `+`.
  # A match naming the receiver's own "." entry (File::FNM_DOTMATCH, or a
  # "." pattern) is `self + "."`, which `+` folds to the receiver.
  def glob(pattern, flags = 0)
    base = @path
    base = base[0, base.length - 2] while base.length > 2 && base.end_with?("#{SEPARATOR}.")
    base = base[0, base.length - 1] while base.length > 1 && base.end_with?(SEPARATOR)
    prefix = base.end_with?(SEPARATOR) ? base : "#{base}#{SEPARATOR}"
    # CRuby globs in the receiver's directory and answers `self + match` for
    # each match, so its "." entries fold the way #+ folds them
    # (`"./."` is the receiver, `"./.hidden"` is `<base>/.hidden`)
    here = base == "." || base == "" || pattern.start_with?(SEPARATOR)
    full = here ? pattern : "#{prefix}#{pattern}"
    Pathname.glob(full, flags).map { |x| self + (here ? x.to_s : x.to_s[prefix.length..]) }
  end

  # Every path under this one, receiver first, depth first. A symlinked
  # directory is yielded but not descended into, so a link back up the tree
  # cannot loop.
  #
  # Written against an explicit stack rather than recursing. The recursive
  # spelling -- `child.find { |x| yield x }` -- is a method that both yields
  # and calls itself through a block forwarding that yield, which Spinel
  # cannot inline and silently emitted as a walk that never descended.
  def find
    stack = [self]
    until stack.empty?
      cur = stack.pop
      yield cur
      next unless cur.directory? && !cur.symlink?

      kids = cur.children
      i = kids.length - 1
      while i >= 0            # reversed, so the stack pops them in order
        stack.push(kids[i])
        i -= 1
      end
    end
    self
  end

  # ---- mutation ----

  def mkdir(mode = 0o777)
    Dir.mkdir(@path, mode)
  end

  def rmdir
    Dir.rmdir(@path)
  end

  # mkdir -p: every missing ancestor, then self. A path that already exists as
  # a directory is not an error.
  def mkpath
    # an empty path names no directory: Dir.mkdir raises what CRuby raises
    Dir.mkdir(@path) if @path.empty?
    descend.each do |dir|
      Dir.mkdir(dir.to_s) unless dir.directory?
    end
    self
  end

  # rm -rf, as CRuby's FileUtils.rm_rf: files and symlinks are unlinked,
  # directories are emptied first, and a path that is not there or an entry
  # that cannot be removed is skipped -- the answer is self either way. Like
  # find, this walks against an explicit stack rather than recursing, so a
  # deep tree costs one rescue frame at a time, not one per level; the
  # entries are listed parent-first and removed in reverse, which puts every
  # child before its directory.
  def rmtree
    stack = [self]
    order = []
    until stack.empty?
      cur = stack.pop
      order.push(cur)
      next unless cur.directory? && !cur.symlink?

      begin
        cur.children.each { |k| stack.push(k) }
      rescue SystemCallError
        # a directory that cannot be listed is left as it is, like rm -rf
      end
    end
    i = order.length - 1
    while i >= 0
      order[i].rmtree_entry
      i -= 1
    end
    self
  end

  # One entry of rmtree: a directory is emptied by then, so rmdir; anything
  # else, a symlink to a directory included, is unlinked. A failure is
  # skipped, as rm -rf skips it; the answer is not used. Protected, so the
  # public surface stays CRuby's.
  protected def rmtree_entry
    if directory? && !symlink?
      Dir.rmdir(@path)
    else
      File.delete(@path)
    end
  rescue SystemCallError
    nil
  end

  # CRuby's order: the directory unlink first, the file unlink on ENOTDIR, so
  # a missing path reports dir_s_rmdir there and here alike.
  def delete
    Dir.unlink(@path)
  rescue Errno::ENOTDIR
    File.unlink(@path)
  end

  def unlink
    delete
  end

  def rename(to)
    File.rename(@path, "#{to}")
  end

  # ---- string helpers, shared by the pure-path methods ----

  # The name components of a path, with separators, "" and "." removed.
  # CRuby's Pathname#plus on the two strings (pathname.rb): path2's names are
  # listed with where each starts; its leading "." names drop, and each
  # leading ".." cancels a name of path1 that is not "." or ".." itself. What
  # is left of path2 is joined, from where its first remaining name starts.
  def self.plus_str(path1, path2)
    prefix2 = path2
    starts2 = []
    names2 = []
    while (r2 = chop_basename(prefix2))
      prefix2 = r2[0]
      starts2.unshift(prefix2.length)
      names2.unshift(r2[1])
    end
    return path2 if prefix2 != ""

    prefix1 = path1
    while true
      while !names2.empty? && names2.first == "."
        starts2.shift
        names2.shift
      end
      r1 = chop_basename(prefix1)
      break unless r1
      prefix1 = r1[0]
      name1 = r1[1]
      next if name1 == "."
      if name1 == ".." || names2.empty? || names2.first != ".."
        prefix1 = prefix1 + name1
        break
      end
      starts2.shift
      names2.shift
    end
    has_name1 = !chop_basename(prefix1).nil?
    # at the root, a ".." has nothing to cancel and drops
    at_root = !has_name1 && File.basename(prefix1).include?(SEPARATOR)
    if at_root
      while !names2.empty? && names2.first == ".."
        starts2.shift
        names2.shift
      end
    end
    if !names2.empty?
      suffix2 = path2[starts2.first..-1]
      has_name1 || at_root ? File.join(prefix1, suffix2) : prefix1 + suffix2
    else
      has_name1 || at_root ? prefix1 : File.dirname(prefix1)
    end
  end

  # [the path before its last name, the last name], or nil for "" and the root
  def self.chop_basename(path)
    base = File.basename(path)
    return nil if base == "" || base == SEPARATOR
    [path[0, path.rindex(base)], base]
  end

  def self.split_str(path)
    out = []
    path.split(SEPARATOR).each do |part|
      out.push(part) unless part == "" || part == "."
    end
    out
  end

  # cleanpath on a raw string: resolve "." and ".." lexically and collapse
  # repeated separators. A leading ".." survives in a relative path (there is
  # no anchor above it to cancel against) but is dropped at an absolute root,
  # where "/.." is "/".
  def self.clean_str(path)
    abs = path.start_with?(SEPARATOR)
    out = []
    split_str(path).each do |part|
      if part == ".."
        if out.empty?
          out.push(part) unless abs
        elsif out[-1] == ".."
          out.push(part)
        else
          out.pop
        end
      else
        out.push(part)
      end
    end
    body = out.join(SEPARATOR)
    return "#{SEPARATOR}#{body}" if abs

    body == "" ? "." : body
  end
end

# Kernel#Pathname(path): the conversion function CRuby mixes into Kernel.
# A Pathname comes back as itself; anything else goes through Pathname.new.
def Pathname(path)
  return path if path.is_a?(Pathname)
  Pathname.new(path)
end
