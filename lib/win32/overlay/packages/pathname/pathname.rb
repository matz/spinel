# lib/win32/overlay: what a native Windows build answers differently, read
# after packages/pathname/pathname.rb. A path's root there is "/" or a
# drive's "C:/", so the methods that look for the root ask root_str.
class Pathname
  # A path's root: "/", a drive's "C:/" on a platform that has drives (it is
  # the platform's File.absolute_path? that says so, so on POSIX a "C:/..."
  # stays the relative path it is there), or "" for a relative path.
  def self.root_str(path)
    return SEPARATOR if path.start_with?(SEPARATOR)
    if path.length >= 3 && path[1] == ":" && path[2] == SEPARATOR && File.absolute_path?(path[0, 3])
      return path[0, 3]
    end
    ""
  end

  # cleanpath on a raw string: resolve "." and ".." lexically and collapse
  # repeated separators. A leading ".." survives in a relative path (there is
  # no anchor above it to cancel against) but is dropped at an absolute root,
  # where "/.." is "/".
  def self.clean_str(path)
    root = root_str(path)
    abs = root != ""
    out = []
    split_str(path[root.length..]).each do |part|
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
    return "#{root}#{body}" if abs

    body == "" ? "." : body
  end

  # self + other. An absolute `other` replaces the receiver entirely, and the
  # result is cleaned, so `Pathname.new("a/b") + ".."` is `a` rather than
  # `a/b/..`.
  def +(other)
    # Every branch answers a Pathname. A method that returns its argument on
    # one path and a fresh object on another has no single type here, and the
    # boxed result made the NEXT `/` in a chain compile as numeric division.
    o = "#{other}"
    return Pathname.new(o) if Pathname.root_str(o) != ""
    return Pathname.new(o) if @path == ""
    return self if o == "" || o == "."

    joined = Pathname.clean_str("#{@path}#{SEPARATOR}#{o}")
    Pathname.new(Pathname.dir_form?(o, joined) ? "#{joined}#{SEPARATOR}" : joined)
  end

  def absolute?
    Pathname.root_str(@path) != ""
  end

  # The Pathnames #descend walks, root first (#ascend walks them reversed),
  # from the root root_str finds: a drive's "C:/" as well as "/". #descend
  # and #ascend themselves are the base's, which answer nil with a block and
  # an Enumerator without one.
  def descend_paths
    out = []
    root = Pathname.root_str(@path)
    parts = Pathname.split_str(@path[root.length..])
    acc = root
    out.push(Pathname.new(root)) if root != ""
    parts.each do |part|
      acc = acc == "" ? part : (acc.end_with?(SEPARATOR) ? "#{acc}#{part}" : "#{acc}#{SEPARATOR}#{part}")
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
    raise ArgumentError, "different prefix" if absolute? != (Pathname.root_str(b) != "")

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
end
