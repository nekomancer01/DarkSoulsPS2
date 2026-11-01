/* EMEVD object ops (ev_obj.h). Semantics from the EMEDF names and DSR's scripts: 2005[07] "Reproduce Object Animation"
 * is soulstruct's EndOfAnimation (Firelink's chest event 11020700 uses it to show an opened chest after a reload), an
 * ObjAct plays ObjActParam objAnimId on its object (Burg doors o1305 / o1312: 0, levers o1200: 1) and its MSB event
 * flag keeps the object in that anim's end state (ASSUMED from the flag ids, TECH_DEBT event-11). */
#include <stdio.h>
#include <string.h>

#include "ev_obj.h"
#include "objects.h"

int ev_obj_anim(int entity, int anim, int loop, float *len)
{
    float l = objects_anim_play(entity, anim, loop);
    if (l < 0)
        return 0;
    if (len)
        *len = l;
    printf("EVT object %d anim %d%s (%.2f s)\n", entity, anim, loop ? " loop" : "", l);
    return 1;
}

int ev_obj_end_anim(int entity, int anim) { return objects_anim_end(entity, anim); }

int ev_obj_enable(int entity, int on) { return objects_entity_enable(entity, on); }

int ev_obj_destroy(int entity) { return objects_entity_break(entity) || objects_entity(entity, NULL, NULL) >= 0; }

int ev_obj_restore(int entity)
{
    if (objects_entity(entity, NULL, NULL) < 0)
        return 0;
    objects_entity_restore(entity);
    return 1;
}

void ev_obj_objact(int part, int anim, int after_load)
{
    if (anim < 0 || part == -1)
        return;
    if (after_load)
        objects_anim_end(part, anim);
    else if (objects_anim_play(part, anim, 0) >= 0)
        printf("EVT object %d anim %d (ObjAct)\n", part, anim);
}

int ev_obj_test(const char *cmd, int *pass, int *fail)
{
    char a[16] = "";
    int id = 0, v = 0, w = 0;
    if (sscanf(cmd, "obj %15s %d %d", a, &id, &v) == 3) {  /* obj anim|end <entity> <anim> */
        int n = !strcmp(a, "anim") ? objects_anim_play(id, v, 0) >= 0 : !strcmp(a, "end") ? objects_anim_end(id, v) : -1;
        printf("TEST ev obj %s %d %d: %d\n", a, id, v, n);
        return n >= 0;
    }
    if (sscanf(cmd, "obj %15s %d", a, &id) == 2) {  /* a breakable / animated object by entity id, as 2005 ops do */
        int n = !strcmp(a, "destroy")   ? objects_entity_break(id)
                : !strcmp(a, "restore") ? objects_entity_restore(id)
                                        : objects_entity_enable(id, !strcmp(a, "enable"));
        printf("TEST ev obj %s %d: %d changed\n", a, id, n);
        return 1;
    }
    if (sscanf(cmd, "expect objanim %d %d %d", &id, &v, &w) == 3) {
        int anim = -2, playing = -1;
        float t = 0;
        int found = objects_anim_state(id, &anim, &t, &playing), ok = found && anim == v && playing == w;
        *pass += ok, *fail += !ok;
        printf("TEST ev expect objanim %d %d %d: anim %d t %.2f playing %d%s %s\n", id, v, w, anim, t, playing,
               found ? "" : " (not found)", ok ? "PASS" : "FAIL");
        return 1;
    }
    if (sscanf(cmd, "expect objbroken %d %d", &id, &v) == 2) {  /* breakable entity id: 1 broken (objects.c) */
        int got = 0, found = objects_entity(id, NULL, &got) >= 0, ok = found && got == v;
        *pass += ok, *fail += !ok;
        printf("TEST ev expect objbroken %d %d: %d%s %s\n", id, v, got, found ? "" : " (not found)", ok ? "PASS" : "FAIL");
        return 1;
    }
    return 0;
}
