import os

src_dir = r'D:\AI Projects\SmoothPan Dwarf Fortress\src'
path = os.path.join(src_dir, 'smoothpan.cpp')

with open(path, 'r') as f:
    content = f.read()

content = '#include <SDL.h>\n' + content

with open(path, 'w') as f:
    f.write(content)
