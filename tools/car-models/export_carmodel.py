# Blender: imports a glTF/GLB car and writes a replacement car model for NFSIISE (model.cmdl + PNG textures).
# Usage: blender -b --python export_carmodel.py -- in.glb out_dir
# The model should face +Z (glTF forward) with Y up, in metres; the game fits its size to the original car.
import bpy, sys, os, struct, bmesh
argv = sys.argv[sys.argv.index('--') + 1:]
src, out = argv[0], argv[1]
os.makedirs(out, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
def mat_params(m):
    p = dict(color=(0.8, 0.8, 0.8, 1.0), metal=0.0, rough=0.5, transmission=0.0, emission=(0, 0, 0), tex='', clearcoat=0.0)
    if not m or not m.use_nodes:
        return p
    bsdf = next((n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if not bsdf:
        return p
    def val(name, default):
        s = bsdf.inputs.get(name)
        return s.default_value if s is not None else default
    c = val('Base Color', (0.8, 0.8, 0.8, 1.0)); p['color'] = tuple(c)
    p['metal'] = float(val('Metallic', 0.0)); p['rough'] = float(val('Roughness', 0.5))
    p['transmission'] = float(val('Transmission Weight', val('Transmission', 0.0)))
    p['clearcoat'] = float(val('Coat Weight', val('Clearcoat', 0.0)))
    a = float(val('Alpha', 1.0)); p['color'] = (c[0], c[1], c[2], a)
    e = val('Emission Color', (0, 0, 0, 1)); es = float(val('Emission Strength', 1.0))
    p['emission'] = (e[0] * es, e[1] * es, e[2] * es)
    link = bsdf.inputs['Base Color'].links
    if link:
        n = link[0].from_node
        while n and n.type != 'TEX_IMAGE' and n.inputs and n.inputs[0].links:
            n = n.inputs[0].links[0].from_node
        if n and n.type == 'TEX_IMAGE' and n.image:
            name = bpy.path.clean_name(n.image.name) + '.png'
            path = os.path.join(out, name)
            if not os.path.exists(path):
                img = n.image.copy(); img.filepath_raw = path; img.file_format = 'PNG'; img.save()
            p['tex'] = name
    return p
mats = {}; batches = {}
for ob in bpy.data.objects:
    if ob.type != 'MESH':
        continue
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    bm = bmesh.new(); bm.from_mesh(me); bmesh.ops.triangulate(bm, faces=bm.faces); bm.to_mesh(me); bm.free()
    me.calc_loop_triangles() if hasattr(me, 'calc_loop_triangles') else None
    M = ob.matrix_world; N = M.to_3x3().inverted().transposed()
    uvl = me.uv_layers.active.data if me.uv_layers.active else None
    for tri in me.loop_triangles:
        m = ob.material_slots[tri.material_index].material if ob.material_slots else None
        key = m.name if m else '_default'
        if key not in mats:
            mats[key] = mat_params(m)
        b = batches.setdefault(key, [])
        for li in tri.loops:
            l = me.loops[li]; v = M @ me.vertices[l.vertex_index].co
            n = (N @ (l.normal if hasattr(l, 'normal') else me.vertices[l.vertex_index].normal)).normalized()
            uv = uvl[li].uv if uvl else (0, 0)
            b.append((v.x, v.y, v.z, n.x, n.y, n.z, uv[0], 1.0 - uv[1]))
    ob.evaluated_get(dg).to_mesh_clear()
# Blender is Z-up: convert to Y-up (x, z, -y)
with open(os.path.join(out, 'model.cmdl'), 'wb') as f:
    keys = [k for k in batches if batches[k]]
    f.write(b'CMDL' + struct.pack('<II', 1, len(keys)))
    for k in keys:
        p = mats[k]; t = p['tex'].encode()[:63]
        f.write(struct.pack('<4f f f f f 3f', *p['color'], p['metal'], p['rough'], p['transmission'], p['clearcoat'], *p['emission']))
        f.write(t + b'\0' * (64 - len(t)))
        verts = batches[k]
        f.write(struct.pack('<I', len(verts)))
        for x, y, z, nx, ny, nz, u, v in verts:
            f.write(struct.pack('<8f', x, z, -y, nx, nz, -ny, u, v))
        print('batch', k, len(verts) // 3, 'tris', p)
