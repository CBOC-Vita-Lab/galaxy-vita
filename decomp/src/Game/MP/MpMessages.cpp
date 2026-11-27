// Galaxy 3.0 multiplayer: the words (see MpMessages.hpp). English, French and Spanish here (kEntries); German and Italian are in
// MpMessagesExtra.cpp, keyed by id. Pure code, no game headers.
// Tone: the game's own system messages (short, friendly, "Yes" / "No"); window texts keep the game's line lengths (about 40).
#include "Game/MP/MpMessages.hpp"

#include <string.h>

namespace MpMsgText {
namespace {

// markup
#define S1 u"\x01"
#define S2 u"\x02"
#define N1 u"\x03"
#define TM u"\x04"
#define P_CROSS u""
#define P_CIRCLE u""
#define P_TRI u""
#define P_SQ u""
#define P_STAR u""

struct Entry {
    MpText id;
    const char* name;          // message window id, or null
    const char16_t* text[LANG_COUNT];
};

const Entry kEntries[] = {
    // ---------------------------------------------------------------- toasts
    {MP_T_JOINED, nullptr, {S1 u" joined.", S1 u" a rejoint la partie.", S1 u" se ha unido."}},
    {MP_T_LEFT, nullptr, {S1 u" left.", S1 u" a quitté la partie.", S1 u" se ha ido."}},
    {MP_T_GOT_STAR, nullptr, {S1 u" got the Power Star!", S1 u" a obtenu une Étoile !", u"¡" S1 u" consiguió la Superestrella!"}},
    {MP_T_FINISHED, nullptr, {S1 u" finished in " TM u"!", S1 u" a fini en " TM u" !", u"¡" S1 u" terminó en " TM u"!"}},
    {MP_T_HOST_CHOOSES, nullptr, {S1 u" chooses the galaxy.", S1 u" choisit la galaxie.", S1 u" elige la galaxia."}},
    {MP_T_HOST_BROWSING, nullptr, {S1 u" is choosing a galaxy.", S1 u" choisit une galaxie.", S1 u" está eligiendo una galaxia."}},
    {MP_T_HOST_BROWSING_STAR, nullptr, {S1 u" is choosing a star.", S1 u" choisit une étoile.", S1 u" está eligiendo una estrella."}},
    {MP_T_PICKED, nullptr, {S1 u" picked " S2 u", Star " N1 u".", S1 u" a choisi " S2 u", étoile " N1 u".",
                            S1 u" eligió " S2 u", estrella " N1 u"."}},
    {MP_T_HOST_DATA_BAD, nullptr, {u"Your data didn't match this game.\nYou left; nothing was changed.",
        u"Vos données ne correspondent pas.\nVous êtes parti ; rien n'a changé.",
        u"Tus datos no coinciden con la partida.\nSaliste; no se cambió nada."}},
    {MP_T_STAR_NOT_SAVED, nullptr, {S1 u"'s Power Star couldn't be saved this time. Sorry!", u"L'Étoile de " S1 u" n'a pas pu être enregistrée. Désolé !",
                                    u"La Superestrella de " S1 u" no se pudo guardar esta vez. ¡Lo sentimos!"}},
    {MP_T_STAMP_0, nullptr, {S1 u": Over here!", S1 u" : Par ici !", S1 u": ¡Por aquí!"}},
    {MP_T_STAMP_1, nullptr, {S1 u": Thanks!", S1 u" : Merci !", S1 u": ¡Gracias!"}},
    {MP_T_STAMP_2, nullptr, {S1 u": Wait for me!", S1 u" : Attendez-moi !", S1 u": ¡Espérame!"}},
    {MP_T_STAMP_3, nullptr, {S1 u": Let's go!", S1 u" : C'est parti !", S1 u": ¡Vamos!"}},
    {MP_T_HOST_MIGRATED, nullptr, {S2 u" left. " S1 u" chooses the galaxy now.", S2 u" a quitté la partie. " S1 u" choisit la galaxie.",
                                   S2 u" se ha ido. Ahora " S1 u" elige la galaxia."}},
    {MP_T_YOU_ARE_HOST, nullptr, {u"You choose the galaxy now.", u"Maintenant, vous choisissez la galaxie.", u"Ahora eliges tú la galaxia."}},
    {MP_T_EVERYONE_LEFT, nullptr, {u"Everyone left.", u"Tout le monde est parti.", u"Todos se han ido."}},
    {MP_T_WARPING, nullptr, {u"Going to " S1 u"...", u"En route vers " S1 u"...", u"Yendo con " S1 u"..."}},
    {MP_T_PLAYER_DOWN, nullptr, {S1 u" fell.", S1 u" a fait une chute.", S1 u" ha caído."}},
    {MP_T_WARPED_TO_YOU, nullptr, {S1 u" warped to you.", S1 u" vous a rejoint.", S1 u" ha venido contigo."}},
    {MP_T_WARP_FAILED, nullptr, {u"Can't warp to " S1 u" right now.", u"Impossible d'aller vers " S1 u" pour le moment.",
                                 u"Ahora no puedes ir con " S1 u"."}},
    {MP_T_NO_SETTINGS, nullptr, {u"Settings can't be opened during play.", u"Réglages indisponibles pendant la partie.",
                                 u"Los ajustes no se abren durante la partida."}},
    {MP_T_SKIP_VOTE, nullptr, {S1 u" wants to skip (" S2 u").", S1 u" veut passer (" S2 u").", S1 u" quiere saltar (" S2 u")."}},
    {MP_T_HOST_RACES_ONLY, nullptr, {u"Only " S1 u" races Cosmic Mario.", u"C'est " S1 u" qui affronte Mario Cosmique.",
                                     u"Solo " S1 u" corre contra Mario Fantasma."}},
    {MP_T_RACE_GO_SOON, nullptr, {u"The race starts soon!", u"La course va commencer !", u"¡La carrera va a empezar!"}},
    {MP_T_BUBBLED, nullptr, {S1 u" is in a bubble!", S1 u" est dans une bulle !", u"¡" S1 u" está en una burbuja!"}},
    {MP_T_POPPED, nullptr, {S1 u" popped " S2 u"'s bubble!", S1 u" a éclaté la bulle de " S2 u" !",
                            u"¡" S1 u" reventó la burbuja de " S2 u"!"}},
    {MP_T_ALL_CAUGHT, nullptr, {u"Everyone was caught! Let's try again.", u"Tout le monde est pris ! On recommence.",
                                u"¡Todos atrapados! A intentarlo otra vez."}},
    {MP_T_OUT_OF_LIVES, nullptr, {S1 u" is out of lives! Pop the bubble!",
                                  S1 u" n'a plus de vies ! Éclatez sa bulle !",
                                  u"¡" S1 u" no tiene vidas! ¡Revienta la burbuja!"}},

    // ---------------------------------------------------------------- Multiplayer menu
    {MP_T_TITLE_MULTI, nullptr, {u"Multiplayer", u"Multijoueur", u"Multijugador"}},
    {MP_T_PLAY_TOGETHER, nullptr, {u"Play Together", u"Jouer ensemble", u"Jugar juntos"}},
    {MP_T_STAR_RACE, nullptr, {u"Star Race", u"Course aux étoiles", u"Carrera estelar"}},
    {MP_T_LEADERBOARDS, nullptr, {u"Leaderboards", u"Classements", u"Clasificaciones"}},
    {MP_T_FRIENDS, nullptr, {u"Friends", u"Amis", u"Amigos"}},
    {MP_T_HOW_TO_PLAY, nullptr, {u"How to Play", u"Comment jouer", u"Cómo jugar"}},
    {MP_T_BACK, nullptr, {u"Back", u"Retour", u"Volver"}},

    {MP_T_INTERNET, nullptr, {u"Internet", u"Internet", u"Internet"}},
    {MP_T_SAME_WIFI, nullptr, {u"Same Wi-Fi", u"Même Wi-Fi", u"Misma red Wi-Fi"}},
    {MP_T_NEARBY, nullptr, {u"Nearby", u"À proximité", u"Cerca"}},

    {MP_T_QUICK_MATCH, nullptr, {u"Quick Match", u"Partie rapide", u"Partida rápida"}},

    {MP_T_PLAYERS, nullptr, {u"Players", u"Joueurs", u"Jugadores"}},
    {MP_T_OFF, nullptr, {u"Off", u"Non", u"No"}},
    {MP_T_ON, nullptr, {u"On", u"Oui", u"Sí"}},
    {MP_T_CODE_ONLY, nullptr, {u"Code Only", u"Avec le code", u"Con código"}},
    {MP_T_VIS_FRIENDS, nullptr, {u"Friends", u"Amis", u"Amigos"}},
    {MP_T_ANYONE, nullptr, {u"Anyone", u"Tout le monde", u"Cualquiera"}},
    {MP_T_FALLS, nullptr, {u"When Someone Falls", u"Si quelqu'un tombe", u"Si alguien cae"}},
    {MP_T_FALLS_BUBBLES, nullptr, {u"Bubble", u"Bulle", u"Burbuja"}},
    {MP_T_FALLS_LIVES, nullptr, {u"Try again", u"Réessayer", u"Reintentar"}},

    {MP_T_TITLE_SAVE, nullptr, {u"Please choose a file.", u"Veuillez choisir un fichier.", u"Elige un archivo."}},
    {MP_T_COOP_FILE, nullptr, {u"Team File " N1, u"Fichier d'équipe " N1, u"Archivo de equipo " N1}},
    {MP_T_NEW, nullptr, {u"New", u"Nouveau", u"Nuevo"}},
    {MP_T_TITLE_SLOT, nullptr, {u"Please choose a Team File.", u"Choisissez un fichier d'équipe.", u"Elige un archivo de equipo."}},
    {MP_T_DESC_SLOT, nullptr, {u"Choose the Team File to play.", u"Choisissez le fichier d'équipe à utiliser.", u"Elige el archivo de equipo."}},
    {MP_T_DESC_SLOT_KEEP, nullptr, {u"Choose where to keep this progress.", u"Choisissez où garder cette progression.", u"Elige dónde guardar este progreso."}},

    {MP_T_OTHER_VERSION, nullptr, {u"Other version", u"Autre version", u"Otra versión"}},
    {MP_T_YOUR_CODE, nullptr, {u"Your friend code: " S1, u"Votre code ami : " S1, u"Tu código de amigo: " S1}},
    {MP_T_ONLINE, nullptr, {u"Online", u"En ligne", u"En línea"}},
    {MP_T_IN_ROOM, nullptr, {u"In a game", u"En partie", u"En una partida"}},
    {MP_T_OFFLINE, nullptr, {u"Offline", u"Hors ligne", u"Desconectado"}},
    {MP_T_JOIN, nullptr, {u"Play with This Friend", u"Jouer avec cet ami", u"Jugar con este amigo"}},   // wire: no Join (a friend's page)
    {MP_T_ADD, nullptr, {u"Add", u"Ajouter", u"Añadir"}},
    {MP_T_REMOVE, nullptr, {u"Remove", u"Retirer", u"Quitar"}},
    {MP_T_NO_FRIENDS, nullptr, {u"No friends yet. Add one with " P_TRI u".", u"Aucun ami. Ajoutez-en un avec " P_TRI u".",
                                u"Aún no hay amigos. Añade uno con " P_TRI u"."}},
    {MP_T_LOADING, nullptr, {u"Loading...", u"Chargement...", u"Cargando..."}},
    {MP_T_LOAD_FAILED, nullptr, {u"Couldn't load the list.", u"Impossible de charger la liste.", u"No se pudo cargar la lista."}},
    {MP_T_RETRY, nullptr, {u"Retry", u"Réessayer", u"Reintentar"}},
    {MP_T_TAB_TOP, nullptr, {u"Top", u"Top", u"Top"}},
    {MP_T_TAB_NEAR, nullptr, {u"Near Me", u"Autour de moi", u"Cerca de mí"}},
    {MP_T_TAB_FRIENDS, nullptr, {u"Friends", u"Amis", u"Amigos"}},
    {MP_T_GALAXY, nullptr, {u"Galaxy", u"Galaxie", u"Galaxia"}},
    {MP_T_STAR, nullptr, {u"Star", u"Étoile", u"Estrella"}},
    {MP_T_NO_TIMES, nullptr, {u"No times yet.", u"Aucun temps pour l'instant.", u"Aún no hay tiempos."}},
    {MP_T_TAB_HINT, nullptr, {u"Choose a galaxy and a star.", u"Choisissez une galaxie et une étoile.", u"Elige una galaxia y una estrella."}},
    {MP_T_STAR_N, nullptr, {u"Star " N1, u"Étoile " N1, u"Estrella " N1}},

    {MP_T_CONNECTING, nullptr, {u"Connecting...", u"Connexion...", u"Conectando..."}},
    {MP_T_CREATING, nullptr, {u"Starting a game...", u"Création de la partie...", u"Creando la partida..."}},
    {MP_T_SEARCHING, nullptr, {u"Looking for a game...", u"Recherche d'une partie...", u"Buscando una partida..."}},
    {MP_T_JOINING, nullptr, {u"Entering the game...", u"Connexion à la partie...", u"Entrando en la partida..."}},
    {MP_T_RECEIVING, nullptr, {u"Getting ready... " N1 u"%", u"Préparation... " N1 u" %", u"Preparando... " N1 u"%"}},
    {MP_T_CANCEL, nullptr, {u"Cancel", u"Annuler", u"Cancelar"}},

    {MP_T_START, nullptr, {u"Start", u"Commencer", u"Empezar"}},
    {MP_T_LEAVE, nullptr, {u"Leave", u"Quitter", u"Salir"}},
    {MP_T_YOU, nullptr, {u"You", u"Vous", u"Tú"}},

    {MP_T_RESUME, nullptr, {u"Back", u"Retour", u"Volver"}},
    {MP_T_WARP, nullptr, {u"Go to a Friend", u"Aller voir un ami", u"Ir con un amigo"}},
    {MP_T_STAMPS, nullptr, {u"Messages", u"Messages", u"Mensajes"}},   // (chat: the Messages page)
    {MP_T_PLAYER_LIST, nullptr, {u"Players", u"Joueurs", u"Jugadores"}},
    {MP_T_BACK_DOME, nullptr, {u"Return to the Observatory", u"Retourner à l'Observatoire", u"Volver al Planetarium"}},
    {MP_T_LEAVE_SESSION, nullptr, {u"Leave Multiplayer",
        u"Quitter le multijoueur",
        u"Salir del multijugador"}},
    {MP_T_TITLE_WARP, nullptr, {u"Warp to...", u"Aller vers...", u"Ir con..."}},
    {MP_T_CLOSE, nullptr, {u"Close", u"Fermer", u"Cerrar"}},
    {MP_T_STAMP_TEXT_0, nullptr, {u"Over here!", u"Par ici !", u"¡Por aquí!"}},
    {MP_T_STAMP_TEXT_1, nullptr, {u"Thanks!", u"Merci !", u"¡Gracias!"}},
    {MP_T_STAMP_TEXT_2, nullptr, {u"Wait for me!", u"Attendez-moi !", u"¡Espérame!"}},
    {MP_T_STAMP_TEXT_3, nullptr, {u"Let's go!", u"C'est parti !", u"¡Vamos!"}},
    {MP_T_SEND, nullptr, {u"Send", u"Envoyer", u"Enviar"}},
    // chat: phrases (receivers see them in their own language: the message carries the number)
    {MP_T_PHRASE_0, nullptr, {u"Over here!", u"Par ici !", u"¡Por aquí!"}},
    {MP_T_PHRASE_1, nullptr, {u"Thanks!", u"Merci !", u"¡Gracias!"}},
    {MP_T_PHRASE_2, nullptr, {u"Wait for me!", u"Attendez-moi !", u"¡Espérame!"}},
    {MP_T_PHRASE_3, nullptr, {u"Let's go!", u"C'est parti !", u"¡Vamos!"}},
    {MP_T_PHRASE_4, nullptr, {u"Hello!", u"Salut !", u"¡Hola!"}},
    {MP_T_PHRASE_5, nullptr, {u"Help!", u"À l'aide !", u"¡Ayuda!"}},
    {MP_T_PHRASE_6, nullptr, {u"Yes!", u"Oui !", u"¡Sí!"}},
    {MP_T_PHRASE_7, nullptr, {u"Which star?", u"Quelle étoile ?", u"¿Qué estrella?"}},
    {MP_T_PHRASE_8, nullptr, {u"Sorry!", u"Désolé !", u"¡Perdón!"}},
    {MP_T_PHRASE_9, nullptr, {u"No!", u"Non !", u"¡No!"}},
    {MP_T_PHRASE_10, nullptr, {u"Let's do this one!", u"On prend celle-ci !", u"¡Vamos con esta!"}},
    {MP_T_PHRASE_11, nullptr, {u"Be right back!", u"Je reviens !", u"¡Ahora vuelvo!"}},
    {MP_T_WRITE, nullptr, {u"Write a Message", u"Écrire un message", u"Escribir un mensaje"}},
    {MP_T_DESC_MESSAGES, nullptr, {u"Choose a message to send.", u"Choisissez un message à envoyer.", u"Elige un mensaje para enviar."}},
    {MP_T_KB_CHAT, nullptr, {u"A short message", u"Un message court", u"Un mensaje corto"}},
    {MP_T_MUTE, nullptr, {u"Hide Messages", u"Masquer les messages", u"Ocultar mensajes"}},
    {MP_T_UNMUTE, nullptr, {u"Show Messages", u"Afficher les messages", u"Mostrar mensajes"}},
    {MP_T_MUTED, nullptr, {u"Hidden", u"Masqués", u"Ocultos"}},
    {MP_T_CHAT_FROM, nullptr, {S1 u": ", S1 u" : ", S1 u": "}},
    {MP_T_WATCHING, nullptr, {u"Watching " S1, u"Vous regardez " S1, u"Mirando a " S1}},
    {MP_T_WATCH_HINT, nullptr, {u"\uE019 or \uE005: watch someone else.", u"\uE019 ou \uE005 : voir un autre joueur.", u"\uE019 o \uE005: ver a otro jugador."}},   // vitabtn: the L and Square glyphs (the Vita's own buttons; the font's Z and "1")
    // entry
    {MP_T_ENTRY_SINGLE, nullptr, {u"Single Player", u"Un joueur", u"Un jugador"}},
    {MP_T_ENTRY_MULTI, nullptr, {u"Multiplayer", u"Multijoueur", u"Multijugador"}},
    {MP_T_TITLE_SPFILE, nullptr, {u"My Files", u"Mes fichiers", u"Mis archivos"}},
    {MP_T_SP_FILE, nullptr, {u"File " N1, u"Fichier " N1, u"Archivo " N1}},
    // touches: the host-rules card (3 lines of at most 44 units: host and mode / the rules / the question) and its toast
    {MP_T_RULES_HOST_T, nullptr, {u"Let's explore together with " S1 u"!", u"Explorons ensemble avec " S1 u" !", u"¡Vamos a explorar juntos con " S1 u"!"}},
    {MP_T_RULES_HOST_R, nullptr, {u"It's a Star Race with " S1 u"!", u"Course aux étoiles avec " S1 u" !", u"¡Carrera estelar con " S1 u"!"}},
    {MP_T_RULES_FALL_BUBBLE, nullptr, {u"If you fall, a bubble takes you to a friend.", u"Si vous tombez, une bulle vous ramène.", u"Si caes, una burbuja te lleva con un amigo."}},
    {MP_T_RULES_FALL_TRY, nullptr, {u"If you fall, just try again!", u"Si vous tombez, réessayez !", u"Si caes, ¡vuelve a intentarlo!"}},
    {MP_T_RULES_RACE, nullptr, {u"Whoever gets the Power Star first wins!", u"Le premier à toucher l'Étoile gagne !", u"¡Gana quien consiga antes la Superestrella!"}},
    {MP_T_RULES_TOAST, nullptr, {u"Rules (" S1 u"): " S2 u".", u"Règles (" S1 u") : " S2 u".", u"Reglas (" S1 u"): " S2 u"."}},
    {MP_T_RULES_CHANGED, nullptr, {S1 u" changed the rules: " S2 u".", S1 u" a changé les règles : " S2 u".", S1 u" cambió las reglas: " S2 u"."}},
    {MP_T_RULES_P_BUBBLES, nullptr, {u"bubbles", u"bulles", u"burbujas"}},
    {MP_T_RULES_P_TRY, nullptr, {u"try again", u"réessayer", u"volver a intentarlo"}},
    {MP_T_LAG, nullptr, {u"Weak signal", u"Signal faible", u"Señal débil"}},

    {MP_T_WAITING_TITLE, nullptr, {u"Waiting for players... " S1, u"En attente des joueurs... " S1, u"Esperando jugadores... " S1}},
    {MP_T_TIME_LEFT, nullptr, {u"Time left", u"Temps restant", u"Tiempo restante"}},
    {MP_T_PLAYING, nullptr, {u"Playing...", u"En course...", u"Jugando..."}},
    {MP_T_RESULTS, nullptr, {u"Results", u"Résultats", u"Resultados"}},
    {MP_T_PLACE_1, nullptr, {u"1st", u"1er", u"1.º"}},
    {MP_T_PLACE_2, nullptr, {u"2nd", u"2e", u"2.º"}},
    {MP_T_PLACE_3, nullptr, {u"3rd", u"3e", u"3.º"}},
    {MP_T_PLACE_4, nullptr, {u"4th", u"4e", u"4.º"}},
    {MP_T_DNF, nullptr, {u"Not finished", u"Non classé", u"No terminó"}},
    {MP_T_CONTINUE, nullptr, {u"Continue", u"Continuer", u"Continuar"}},
    {MP_T_CONTINUE_IN, nullptr, {u"Continue (" N1 u")", u"Continuer (" N1 u")", u"Continuar (" N1 u")"}},
    {MP_T_OK, nullptr, {u"OK", u"OK", u"Aceptar"}},
    {MP_T_SELECT, nullptr, {u"OK",
        u"OK",
        u"Aceptar"}},
    {MP_T_CHANGE, nullptr, {u"Change", u"Modifier", u"Cambiar"}},
    {MP_T_TAB, nullptr, {u"Next List",
        u"Liste suivante",
        u"Otra lista"}},
    {MP_T_KB_NAME, nullptr, {u"Your name", u"Votre nom", u"Tu nombre"}},
    {MP_T_KB_FRIEND, nullptr, {u"Friend code", u"Code ami", u"Código de amigo"}},
    {MP_T_FILESEL_ICON, nullptr, {u"Multi-\nplayer", u"Multi-\njoueur", u"Multi-\njugador"}},
    {MP_T_FILESEL_BALLOON, nullptr, {u"Multiplayer", u"Multijoueur", u"Multijugador"}},
    {MP_T_SESSION_TITLE, nullptr, {u"Multiplayer", u"Multijoueur", u"Multijugador"}},
    {MP_T_HOST_TAG, nullptr, {u"\uE007", u"\uE007", u"\uE007"}},
    {MP_T_TITLE_CHARS, nullptr, {u"Choose Your Character", u"Choisissez un personnage", u"Elige un personaje"}},
    {MP_T_CHAR_MARIO, nullptr, {u"Mario", u"Mario", u"Mario"}},
    {MP_T_CHAR_LUIGI, nullptr, {u"Luigi", u"Luigi", u"Luigi"}},
    {MP_T_CHAR_TOAD, nullptr, {u"Toad", u"Toad", u"Toad"}},
    // (the game's own words for the Cosmic Comet: "Comète Cosmique" / "Cometa Fantasma"; its races in Spanish: "¡Fantasma!")
    {MP_T_CHAR_COSMIC, nullptr, {u"Cosmic Mario", u"Mario Cosmique", u"Mario Fantasma"}},
    {MP_T_CHAR_YELLOW_TOAD, nullptr, {u"Yellow Toad", u"Toad jaune", u"Toad amarillo"}},
    {MP_T_CHAR_COSMIC_LUIGI, nullptr, {u"Cosmic Luigi", u"Luigi Cosmique", u"Luigi Fantasma"}},
    {MP_T_DESC_CHARS, nullptr, {u"Each player plays a different character.", u"Chaque joueur a son propre personnage.",
                                u"Cada jugador lleva un personaje distinto."}},
    {MP_T_TAKEN, nullptr, {u"Already taken", u"Déjà pris", u"Ya elegido"}},
    {MP_T_CHANGE_CHAR, nullptr, {u"Character", u"Personnage", u"Personaje"}},
    {MP_T_SAVE_TO_MY_FILE, nullptr, {u"Back to My File " N1, u"Dans mon fichier " N1, u"En mi archivo " N1}},
    {MP_T_DESC_SLOT_LEAVE, nullptr, {u"Choose where to save this progress.", u"Choisissez où sauvegarder cette progression.",
                                     u"Elige dónde guardar este progreso."}},
    {MP_T_DESC_SLOT_RECOVER, nullptr, {u"Choose where to keep your last game.", u"Choisissez où garder votre dernière partie.",
                                       u"Elige dónde guardar tu última partida."}},
    {MP_T_WAIT_CHAR, nullptr, {u"Just a moment...", u"Un instant...", u"Un momento..."}},
    {MP_T_TEAM_TOTAL, nullptr, {u"Team", u"Équipe", u"Equipo"}},
    {MP_T_RACE_AGAIN, nullptr, {u"Race Again", u"Nouvelle course", u"Otra carrera"}},
    {MP_T_NEXT_STAR, nullptr, {u"Next Star", u"Étoile suivante", u"Siguiente estrella"}},
    {MP_T_LEAVE_RACE, nullptr, {u"Leave Race", u"Quitter la course", u"Salir de la carrera"}},
    {MP_T_RACE_HOST_PICKS, nullptr, {S1 u" is choosing...", S1 u" choisit...", S1 u" está eligiendo..."}},
    {MP_T_RACE_ASK, nullptr, {u"Race Again?", u"Rejouer ?", u"¿Otra carrera?"}},
    {MP_T_RACE_ASKED, nullptr, {u"Asked! Waiting for " S1 u"...", u"Demandé ! " S1 u" décide...", u"¡Pedido! Esperando a " S1 u"..."}},
    {MP_T_RACE_WANTS, nullptr, {S1 u" wants to race again", S1 u" veut rejouer", S1 u" quiere otra carrera"}},
    {MP_T_HEAD_START, nullptr, {u"Late start: " N1 u" s", u"Départ retardé : " N1 u" s", u"Salida tardía: " N1 u" s"}},
    {MP_T_SERIES, nullptr, {u"Series", u"Série", u"Serie"}},
    // wire: the Friends screens (were English placeholders in MpMenuFriends.cpp)
    {MP_T_MY_FRIEND_CODE, nullptr, {u"Your Friend Code", u"Votre code ami", u"Tu código de amigo"}},
    {MP_T_FRIEND_MARK, nullptr, {u"Friend", u"Ami", u"Amigo"}},
    {MP_T_FRIEND_CODE_LINE, nullptr, {u"Friend Code: " S1, u"Code ami : " S1, u"Código de amigo: " S1}},
    {MP_T_REMOVE_FRIEND, nullptr, {u"Remove Friend", u"Retirer cet ami", u"Quitar amigo"}},
    {MP_T_DESC_FRIEND_PAGE, nullptr, {u"Play with them, or see their friend code.",
        u"Jouez avec cet ami, ou voyez son code ami.",
        u"Juega con tu amigo o mira su código."}},
    {MP_T_DESC_FRIEND_JOIN, nullptr, {u"Play in their game.", u"Jouez dans sa partie.", u"Juega en su partida."}},
    {MP_T_DESC_FRIEND_REMOVE, nullptr, {u"They won't be your friend any more.", u"Vous ne serez plus amis.", u"Ya no serán amigos."}},
    {MP_T_SVC_BUSY, nullptr, {u"Online play is busy right now. Please try again later.", u"Jeu en ligne très sollicité. Veuillez réessayer plus tard.", u"El juego en línea está muy ocupado. Inténtalo más tarde."}},
    {MP_T_SVC_OFFLINE, nullptr, {u"Online play isn't available yet.", u"Le jeu en ligne n'est pas encore disponible.", u"El juego en línea aún no está disponible."}},
    {MP_T_TEAM_STAR, nullptr, {u"TEAM STAR!", u"ÉTOILE D'ÉQUIPE !", u"¡SUPERESTRELLA EN EQUIPO!"}},
    {MP_T_RECONNECTING, nullptr, {u"Reconnecting...", u"Reconnexion...", u"Reconectando..."}},
    // netx: internet play by address (no online service: the host's address is typed in)

    // ---------------------------------------------------------------- friend requests, blocked players (friendsui)
    {MP_T_REQUESTS, nullptr, {u"Requests (" N1 u")", u"Demandes (" N1 u")", u"Solicitudes (" N1 u")"}},
    {MP_T_TITLE_REQUESTS, nullptr, {u"Friend Requests", u"Demandes d'ami", u"Solicitudes de amistad"}},
    {MP_T_TITLE_REQUEST, nullptr, {u"Friend Request", u"Demande d'ami", u"Solicitud de amistad"}},
    {MP_T_NOT_NOW, nullptr, {u"Not Now", u"Pas maintenant", u"Ahora no"}},
    {MP_T_BLOCK, nullptr, {u"Block", u"Bloquer", u"Bloquear"}},
    {MP_T_UNBLOCK, nullptr, {u"Unblock", u"Débloquer", u"Desbloquear"}},
    {MP_T_BY_CODE, nullptr, {u"By Code", u"Par code", u"Por código"}},
    {MP_T_BLOCKED_PLAYERS, nullptr, {u"Blocked Players", u"Joueurs bloqués", u"Jugadores bloqueados"}},
    {MP_T_DESC_REQUESTS, nullptr, {u"These players added you. Choose one.",
        u"Ils vous ont ajouté. Choisissez-en un.",
        u"Estos jugadores te añadieron. Elige uno."}},
    {MP_T_DESC_BLOCKED, nullptr, {u"Unblocked players can send requests again.", u"Un joueur débloqué peut refaire une demande.", u"Al desbloquear, podrá enviarte solicitudes."}},
    {MP_T_DESC_REQ_ADD, nullptr, {u"Become friends.", u"Devenez amis.", u"Hacerse amigos."}},
    {MP_T_DESC_NOT_NOW, nullptr, {u"Dismiss this request. They can ask again.",
        u"Ignorer la demande. Il pourra la renvoyer.",
        u"Descartar la solicitud. Podrá reenviarla."}},
    {MP_T_DESC_BLOCK, nullptr, {u"No more requests from this player.",
        u"Plus aucune demande de ce joueur.",
        u"No recibirás más solicitudes suyas."}},
    {MP_T_DESC_UNBLOCK, nullptr, {u"Allow this player to ask again.", u"Autoriser ce joueur à demander de nouveau.",
                                  u"Permitir que este jugador vuelva a pedirlo."}},
    {MP_T_NO_BLOCKED, nullptr, {u"No one blocked from this console yet.", u"Personne n'est bloqué depuis cette console.",
                                u"Nadie bloqueado desde esta consola."}},
    {MP_T_KB_UNBLOCK, nullptr, {u"Code to unblock", u"Code à débloquer", u"Código a desbloquear"}},

    // ---------------------------------------------------------------- Connection Test (conntest)

    // ---------------------------------------------------------------- message windows (one clear sentence each)
    {MP_T_ERR_NO_WIFI, "MP_ERR_NO_WIFI", {
        u"Couldn't connect to Wi-Fi.\nCheck your Wi-Fi settings.",
        u"Connexion Wi-Fi impossible.\nVérifiez vos paramètres Wi-Fi.",
        u"No se pudo conectar a la red Wi-Fi.\nRevisa los ajustes de Wi-Fi."}},
    {MP_T_ERR_SERVICE_DOWN, "MP_ERR_SERVICE_DOWN", {u"The online service isn't available\nright now. Please try again later.\nFriends in the same room can still play.",
        u"Le service en ligne est indisponible.\nVeuillez réessayer plus tard.\nLes amis dans la même pièce peuvent jouer.",
        u"El servicio en línea no está disponible.\nInténtalo de nuevo más tarde.\nCon amigos en la misma habitación sí puedes."}},
    {MP_T_ERR_UPDATE, "MP_ERR_UPDATE", {
        u"A newer version of the game is needed\nto play online.",
        u"Une version plus récente du jeu est\nnécessaire pour jouer en ligne.",
        u"Se necesita una versión más reciente\ndel juego para jugar en línea."}},
    {MP_T_ERR_ROOM_NOT_FOUND, "MP_ERR_ROOM_NOT_FOUND", {
        u"That game couldn't be found.\nCheck the code and try again.",
        u"Cette partie est introuvable.\nVérifiez le code et réessayez.",
        u"No se encontró la partida.\nRevisa el código e inténtalo otra vez."}},
    {MP_T_ERR_ROOM_FULL, "MP_ERR_ROOM_FULL", {u"That game already has four players.\nTry again when someone leaves.",
        u"Cette partie a déjà quatre joueurs.\nRéessayez quand quelqu'un part.",
        u"Esa partida ya tiene cuatro jugadores.\nInténtalo cuando alguien se vaya."}},
    {MP_T_ERR_VERSION, "MP_ERR_VERSION", {u"Your game and your friend's game are\ndifferent versions. Please update both,\nthen try again.",
        u"Votre jeu et celui de votre ami n'ont\npas la même version. Mettez-les à jour,\npuis réessayez.",
        u"Tu juego y el de tu amigo son de\nversiones distintas. Actualicen ambos\ne inténtalo de nuevo."}},
    {MP_T_ERR_TIMEOUT, "MP_ERR_TIMEOUT", {
        u"That took too long.\nPlease try again.", u"Cela a pris trop de temps.\nVeuillez réessayer.", u"Ha tardado demasiado.\nInténtalo de nuevo."}},
    {MP_T_ERR_HOST_LEFT, "MP_ERR_HOST_LEFT", {
        u"This game has ended.", u"Cette partie est terminée.", u"La partida ha terminado."}},
    {MP_T_ERR_KICKED, "MP_ERR_KICKED", {
        u"The game went on without you.\nSee you next time!", u"La partie continue sans vous.\nÀ bientôt !", u"La partida sigue sin ti.\n¡Hasta la próxima!"}},
    {MP_T_ERR_NAT, "MP_ERR_NAT", {u"We couldn't reach that player from here.\nTry again, or play in the same room.",
        u"Impossible d'atteindre ce joueur d'ici.\nRéessayez, ou jouez dans la même pièce.",
        u"No pudimos llegar a ese jugador desde\naquí. Inténtalo de nuevo, o juega en la\nmisma habitación."}},
    {MP_T_ERR_NAME_TAKEN, "MP_ERR_NAME_TAKEN", {
        u"That name is already taken.\nPlease choose another one.",
        u"Ce nom est déjà pris.\nVeuillez en choisir un autre.",
        u"Ese nombre ya está en uso.\nElige otro, por favor."}},
    {MP_T_ERR_BAD_NAME, "MP_ERR_BAD_NAME", {
        u"That name can't be used.\nPlease choose another one.",
        u"Ce nom ne peut pas être utilisé.\nVeuillez en choisir un autre.",
        u"No se puede usar ese nombre.\nElige otro, por favor."}},
    {MP_T_ERR_RATE, "MP_ERR_RATE", {
        u"Too many tries in a short time.\nPlease wait a moment and try again.",
        u"Trop d'essais en peu de temps.\nPatientez un peu et réessayez.",
        u"Demasiados intentos seguidos.\nEspera un momento y vuelve a intentarlo."}},
    {MP_T_ERR_INTERNAL, "MP_ERR_INTERNAL", {
        u"Something went wrong.\nPlease try again.",
        u"Un problème est survenu.\nVeuillez réessayer.",
        u"Algo salió mal.\nInténtalo de nuevo."}},
    {MP_T_DLG_LEAVE_GUEST, "MP_LEAVE_GUEST", {
        u"Leave the game?", u"Voulez-vous quitter la partie ?", u"¿Quieres salir de la partida?"}},
    {MP_T_DLG_LEAVE_HOST, "MP_LEAVE_HOST", {
        u"Leave the game?\nThe others can keep playing.",
        u"Voulez-vous quitter la partie ?\nLes autres pourront continuer.",
        u"¿Quieres salir de la partida?\nLos demás podrán seguir jugando."}},
    {MP_T_DLG_CLOSE_ROOM, "MP_CLOSE_ROOM", {
        u"End the game for everyone?",
        u"Voulez-vous terminer la partie\npour tout le monde ?",
        u"¿Quieres terminar la partida\npara todos?"}},
    {MP_T_DLG_KICK, "MP_KICK", {
        u"Remove " S1 u" from the game?", u"Retirer " S1 u" de la partie ?", u"¿Sacar a " S1 u" de la partida?"}},
    {MP_T_DLG_PLAY_MINE, "MP_PLAY_MINE", {
        u"Your game will save to your file " N1 u".\nA backup is made first. Is this OK?",
        u"La partie sauvegardera dans votre\nfichier " N1 u". Une copie est faite avant.\nD'accord ?",
        u"La partida se guardará en tu archivo " N1 u".\nAntes se hace una copia. ¿De acuerdo?"}},
    {MP_T_DLG_OVERWRITE, "MP_OVERWRITE", {
        u"Team File " N1 u" will be replaced.\nIs this OK?",
        u"Le fichier d'équipe " N1 u" sera remplacé.\nVoulez-vous continuer ?",
        u"El archivo de equipo " N1 u" se\nsustituirá. ¿Quieres seguir?"}},
    {MP_T_DLG_KEEP, "MP_KEEP", {
        u"Save this progress to a Team File?",
        u"Garder cette progression dans\nun fichier d'équipe ?",
        u"¿Quieres guardar este progreso\nen un archivo de equipo?"}},
    {MP_T_DLG_KEPT, "MP_KEPT", {
        u"The progress was saved in\nTeam File " N1 u".",
        u"La progression a été sauvegardée\ndans le fichier d'équipe " N1 u".",
        u"El progreso se guardó en\nel archivo de equipo " N1 u"."}},
    {MP_T_DLG_KEEP_FAILED, "MP_KEEP_FAILED", {
        u"The progress couldn't be saved.", u"La progression n'a pas pu être\nsauvegardée.", u"No se pudo guardar el progreso."}},
    {MP_T_DLG_FRIEND_ADDED, "MP_FRIEND_ADDED", {u"You added " S1 u" as a friend.\nYou'll be friends once they add you too.",
        u"Vous avez ajouté " S1 u" comme ami.\nVous serez amis quand il vous ajoutera.",
        u"Añadiste a " S1 u" como amigo.\nSerán amigos cuando también te añada."}},
    {MP_T_DLG_FRIEND_REMOVE, "MP_FRIEND_REMOVE", {
        u"Remove " S1 u" from your friends?", u"Retirer " S1 u" de vos amis ?", u"¿Quitar a " S1 u" de tus amigos?"}},
    {MP_T_DLG_BACK_DOME, "MP_BACK_DOME", {
        u"Take everyone back to the Observatory?", u"Ramener tout le monde à l'Observatoire ?",
        u"¿Llevar a todos de vuelta\nal Planetarium?"}},
    {MP_T_DLG_EVERYONE_LEFT, "MP_EVERYONE_LEFT", {
        u"Everyone else left the game.", u"Tous les autres joueurs sont partis.", u"Todos los demás han salido."}},
    {MP_T_DLG_POWER_SAVE, "MP_POWER_SAVE", {u"Tip: for smoother play, keep Wi-Fi on in\nSettings > Power Save Settings.", u"Astuce : pour mieux jouer, gardez le Wi-Fi\nactif dans Paramètres > Économie d'énergie.", u"Consejo: para jugar mejor, deja el Wi-Fi\nactivo en Ajustes > Ahorro de energía."}},
    {MP_T_DLG_SAVE_FAILED, "MP_SAVE_FAILED", {
        u"The save couldn't be prepared.", u"La sauvegarde n'a pas pu être préparée.", u"No se pudo preparar el archivo."}},
    {MP_T_DLG_NO_ROOM_FILE, "MP_NO_ROOM_FILE", {
        u"Please choose a file first.", u"Veuillez d'abord choisir un fichier.", u"Primero elige un archivo."}},
    {MP_T_DLG_FRIEND_FAILED, "MP_FRIEND_FAILED", {
        u"That friend code couldn't be added.", u"Ce code ami n'a pas pu être ajouté.", u"No se pudo añadir ese código."}},
    {MP_T_DLG_SAVE_ASK, "MP_SAVE_ASK", {
        u"Save your progress?", u"Voulez-vous sauvegarder ?", u"¿Quieres guardar?"}},
    {MP_T_DLG_FEWER_STARS, "MP_FEWER_STARS", {
        u"Team File " N1 u" has more Power Stars.\nReplace it anyway?",
        u"Le fichier d'équipe " N1 u" a plus\nd'Étoiles. Le remplacer quand même ?",
        u"El archivo de equipo " N1 u" tiene más\nEstrellas. ¿Quieres sustituirlo?"}},
    {MP_T_DLG_RECOVERY, "MP_RECOVERY", {
        u"Your last game ended suddenly.\nKeep its progress?",
        u"Votre dernière partie s'est\ninterrompue brusquement.\nGarder sa progression ?",
        u"Tu última partida terminó de\nforma inesperada.\n¿Guardar su progreso?"}},
    {MP_T_DLG_SAVED, "MP_SAVED", {
        u"Your progress was saved.", u"Votre progression a été sauvegardée.", u"Se guardó tu progreso."}},
    {MP_T_DLG_SAVE_MINE, "MP_SAVE_MINE", {
        u"Save this progress to your file " N1 u"?\nA backup is made first.",
        u"Sauvegarder dans votre fichier " N1 u" ?\nUne copie est faite avant.",
        u"¿Guardar en tu archivo " N1 u"?\nAntes se hace una copia."}},
    {MP_T_DLG_IMPORT_FEWER, "MP_IMPORT_FEWER", {
        u"Team File " N1 u" has more Power Stars\nthan your file. Replace it?",
        u"Le fichier d'équipe " N1 u" a plus\nd'Étoiles que le vôtre. Le remplacer ?",
        u"El archivo de equipo " N1 u" tiene más\nEstrellas que el tuyo. ¿Sustituirlo?"}},
    {MP_T_DLG_SP_UNAVAILABLE, "MP_SP_UNAVAILABLE", {
        u"Your single-player file can't be\nopened right now. Your progress\nmay not be saved.",
        u"Votre fichier solo ne peut pas être\nouvert pour le moment. La progression\npourrait ne pas être sauvegardée.",
        u"Tu archivo de un jugador no se puede\nabrir ahora mismo. Puede que tu progreso\nno se guarde."}},
    // netx
    {MP_T_DLG_ADDR_NO_ANSWER, "MP_ADDR_NO_ANSWER", {
        u"Your friend can't be reached.\nPlease check the code, or ask your\nfriend to start the game instead.",
        u"Impossible de joindre votre ami.\nVérifiez le code, ou demandez à\nvotre ami de commencer la partie.",
        u"No se puede llegar a tu amigo.\nRevisa el código o pídele a tu\namigo que empiece la partida."}},
    {MP_T_DLG_REQ_BLOCK, "MP_REQ_BLOCK", {
        u"Block " S1 u"?\nThey won't be able to send you requests.",
        u"Bloquer " S1 u" ?\nCe joueur ne pourra plus vous\nenvoyer de demandes.",
        u"¿Bloquear a " S1 u"?\nYa no podrá enviarte solicitudes."}},
    {MP_T_DLG_REQ_ACCEPTED, "MP_REQ_ACCEPTED", {
        S1 u" is now your friend!", S1 u" est maintenant votre ami !", u"¡" S1 u" ahora es tu amigo!"}},
    {MP_T_DLG_REQ_BLOCKED, "MP_REQ_BLOCKED", {
        S1 u" is blocked.\nYou can undo this in Blocked Players.",
        S1 u" est bloqué.\nVous pouvez annuler dans\nJoueurs bloqués.",
        S1 u" está bloqueado.\nPuedes deshacerlo en\nJugadores bloqueados."}},
    {MP_T_DLG_UNBLOCKED, "MP_UNBLOCKED", {
        S1 u" was unblocked.", S1 u" a été débloqué.", S1 u" fue desbloqueado."}},
    {MP_T_DLG_REQ_FAILED, "MP_REQ_FAILED", {
        u"That didn't work. Try again later.", u"Cela n'a pas fonctionné.\nRéessayez plus tard.", u"No funcionó. Inténtalo más tarde."}},
    // fix3: the online service's refusals
    {MP_T_ERR_BANNED, "MP_ERR_BANNED", {u"Online play isn't available for this\naccount right now.",
        u"Le jeu en ligne n'est pas disponible\npour ce compte pour le moment.",
        u"El juego en línea no está disponible\npara esta cuenta por ahora."}},
    {MP_T_ERR_FRIENDS_FULL, "MP_ERR_FRIENDS_FULL", {
        u"Your friend list is full.\nRemove a friend to add another.",
        u"Votre liste d'amis est pleine.\nRetirez un ami pour en ajouter un autre.",
        u"Tu lista de amigos está llena.\nQuita a un amigo para añadir otro."}},
    {MP_T_ERR_NOT_FRIENDS, "MP_ERR_NOT_FRIENDS", {
        u"You can play with this player once\nyou have added each other as friends.",
        u"Vous pourrez jouer avec ce joueur\nquand vous serez amis tous les deux.",
        u"Podrás jugar con este jugador cuando\nlos dos se hayan añadido como amigos."}},
    {MP_T_ERR_NO_PLAYER, "MP_ERR_NO_PLAYER", {
        u"No player has that friend code.\nCheck it and try again.",
        u"Aucun joueur n'a ce code ami.\nVérifiez-le et réessayez.",
        u"Ningún jugador tiene ese código\nde amigo. Revísalo e inténtalo\notra vez."}},
    {MP_T_ERR_OWN_CODE, "MP_ERR_OWN_CODE", {
        u"That's your own friend code.\nShare it with your friends!",
        u"C'est votre propre code ami.\nPartagez-le avec vos amis !",
        u"Ese es tu propio código de amigo.\n¡Compártelo con tus amigos!"}},
    {MP_T_ERR_NOT_MEMBER, "MP_ERR_NOT_MEMBER", {
        u"You were away for too long, so\nthe game went on without you.",
        u"Vous avez été absent trop longtemps.\nLa partie a continué sans vous.",
        u"Estuviste ausente demasiado tiempo\ny la partida siguió sin ti."}},
    {MP_T_ERR_DAILY, "MP_ERR_DAILY", {u"Online play is very busy today.\nPlease try again tomorrow.\nFriends in the same room can still play.",
        u"Le jeu en ligne est très sollicité.\nVeuillez réessayer demain.\nVous pouvez jouer avec vos amis à côté.",
        u"El juego en línea está muy ocupado hoy.\nInténtalo de nuevo mañana.\nCon amigos en la misma habitación sí puedes."}},
    // quota32
    {MP_T_ERR_BUSY, "MP_ERR_BUSY", {u"Online play is busy right now.\nPlease try again later.\nFriends in the same room can still play.",
        u"Le jeu en ligne est très sollicité.\nVeuillez réessayer plus tard.\nVous pouvez jouer avec vos amis à côté.",
        u"El juego en línea está muy ocupado.\nInténtalo de nuevo más tarde.\nCon amigos en la misma habitación sí puedes."}},
    // gamefix
    {MP_T_DLG_SP_FEWER, "MP_SP_FEWER", {
        u"Your file " N1 u" has more Power Stars,\nor was saved more recently.\nReplace it anyway?",
        u"Votre fichier " N1 u" a plus d'Étoiles ou\na été sauvegardé plus récemment.\nLe remplacer quand même ?",
        u"Tu archivo " N1 u" tiene más Estrellas\no se guardó más recientemente.\n¿Quieres sustituirlo?"}},
    // ---------------------------------------------------------------- flow (wave 2b): the simpler menus
    {MP_T_REJOIN, nullptr, {u"Continue " S1 u"'s Game",
        u"Reprendre la partie de " S1,
        u"Seguir en la partida de " S1}},
    {MP_T_SCAN_NEARBY, nullptr, {u"Look for Friends Nearby", u"Chercher des amis proches", u"Buscar amigos cercanos"}},
    {MP_T_SCAN_WIFI, nullptr, {u"Look for Friends on Wi-Fi",
        u"Chercher des amis en Wi-Fi",
        u"Buscar amigos por Wi-Fi"}},
    {MP_T_NEW_COOP_FILE, nullptr, {u"New Team File", u"Nouveau fichier d'équipe", u"Nuevo archivo de equipo"}},
    {MP_T_OWN_FILE, nullptr, {u"Start from My File", u"Partir de mon fichier...", u"Empezar desde mi archivo..."}},
    {MP_T_DESC_NEW_COOP, nullptr, {u"A new team adventure. Your files stay as is.",
        u"Une nouvelle aventure. Vos fichiers restent.",
        u"Una aventura nueva. Tus archivos no cambian."}},
    {MP_T_DESC_OWN_FILE, nullptr, {u"Continue from one of your own files.", u"Continuer depuis un de vos fichiers.", u"Sigue desde uno de tus archivos."}},
    {MP_T_DESC_COOP_N, nullptr, {u"Team File " N1 u". Your adventure together.",
        u"Fichier d'équipe " N1 u". Votre aventure.",
        u"Archivo de equipo " N1 u". Progreso en equipo."}},
    {MP_T_LAST_PLAYED, nullptr, {u"Last played: ", u"Dernière partie : ", u"Última partida: "}},
    {MP_T_DESC_SPFILE_PICK, nullptr, {u"Choose the file to start from.", u"Choisissez le fichier de départ.", u"Elige el archivo de partida."}},
    {MP_T_USE_COPY, nullptr, {u"Play on a Copy", u"Jouer avec une copie", u"Jugar con una copia"}},
    {MP_T_USE_PLAY, nullptr, {u"Play This File", u"Jouer avec ce fichier", u"Jugar con este archivo"}},
    {MP_T_PLAYED_WITH, nullptr, {u"Played With (" N1 u")", u"Joué avec (" N1 u")", u"Jugaste con (" N1 u")"}},
    {MP_T_TITLE_PLAYED, nullptr, {u"Played With", u"Joué avec", u"Jugaste con"}},
    {MP_T_DESC_PLAYED, nullptr, {u"Recent players. Add them as friends.",
        u"Vos derniers partenaires. Ajoutez-les.",
        u"Jugadores recientes. Añádelos."}},
    {MP_T_CHANGE_CHAR_LONG, nullptr, {u"Change Character", u"Changer de personnage", u"Cambiar de personaje"}},
    {MP_T_ADD_FRIEND, nullptr, {u"Add Friend", u"Ajouter en ami", u"Añadir amigo"}},
    {MP_T_REMOVE_ROOM, nullptr, {u"Remove from Game", u"Retirer de la partie", u"Sacar de la partida"}},
    // flow (one door): the gathering screen (the line in the game's bottom message; \uE018 = START), its buttons, the falls question
    // m-conntest: "Who Can I Play With?"
    {MP_T_CT_TITLE, nullptr, {u"Who Can I Play With?", u"Avec qui puis-je jouer ?", u"¿Con quién puedo jugar?"}},
    {MP_T_CT_Q_ROOM, nullptr, {u"People in this room", u"Gens dans la pièce", u"En esta habitación"}},
    {MP_T_CT_Q_FAR, nullptr, {u"A friend far away", u"Un ami lointain", u"Un amigo lejano"}},
    {MP_T_CT_Q_ONLINE, nullptr, {u"Anyone online", u"Joueurs en ligne", u"Cualquiera en línea"}},
    {MP_T_CT_V_GOOD, nullptr, {u"Good", u"Bon", u"Bien"}},
    {MP_T_CT_V_CHOPPY, nullptr, {u"May be choppy", u"Parfois saccadé", u"Puede cortarse"}},
    {MP_T_CT_V_NOT_NOW, nullptr, {u"Not right now", u"Pas pour le moment", u"Ahora no"}},
    {MP_T_CT_CHECKING, nullptr, {u"Checking...", u"Vérification...", u"Comprobando..."}},
    {MP_T_CT_AGAIN, nullptr, {u"Check Again", u"Revérifier", u"Comprobar de nuevo"}},
    {MP_T_CT_WAIT_ROOM, nullptr, {u"Looking for friends in this room...", u"Recherche d'amis dans cette pièce...", u"Buscando amigos en esta habitación..."}},
    {MP_T_CT_WAIT_FAR, nullptr, {u"Checking if a faraway friend can play...", u"Vérification pour un ami lointain...", u"Probando si un amigo lejano puede jugar..."}},
    {MP_T_CT_WAIT_ONLINE, nullptr, {u"Checking online play...", u"Vérification du jeu en ligne...", u"Comprobando el juego en línea..."}},
    {MP_T_CT_TOOK_LONG, nullptr, {u"That took too long.", u"Cela a pris trop de temps.", u"Esto ha tardado demasiado."}},
    {MP_T_CT_L_CLOSER, nullptr, {u"Move closer to your Wi-Fi.", u"Rapprochez-vous de votre Wi-Fi.", u"Acércate a tu Wi-Fi."}},
    {MP_T_CT_L_WIFI, nullptr, {u"Connect to Wi-Fi in the system settings.",
        u"Connectez-vous au Wi-Fi (réglages système).",
        u"Conéctate al Wi-Fi en los ajustes."}},
    {MP_T_CT_L_SEE, nullptr, {u"Friends may not see you on this Wi-Fi.", u"Vos amis ne vous verront peut-être pas sur ce Wi-Fi.", u"Tus amigos quizá no te vean en este Wi-Fi."}},
    {MP_T_CT_L_FAR_MAYBE, nullptr, {u"May not work from here. Let a friend start the game.", u"Pas sûr d'ici. Laissez un ami commencer la partie.", u"Puede no funcionar aquí. Deja que un amigo empiece."}},
    {MP_T_CT_L_FAR_NO, nullptr, {u"Friends can't reach you here. Let a friend start the game.", u"Vos amis ne peuvent pas vous joindre. Laissez un ami commencer.", u"Tus amigos no pueden llegar a ti. Deja que un amigo empiece."}},
    {MP_T_CT_L_SLOW, nullptr, {u"Playing in the same room will be smoother.", u"Jouer dans la même pièce sera plus fluide.", u"Jugar en la misma habitación irá mejor."}},
    {MP_T_CT_L_DOWN, nullptr, {u"Check your Wi-Fi, or try again later.", u"Vérifiez votre Wi-Fi ou réessayez plus tard.", u"Comprueba tu Wi-Fi o inténtalo más tarde."}},
    {MP_T_CT_L_UPDATE, nullptr, {u"Please update the game to play online.", u"Mettez le jeu à jour pour jouer en ligne.", u"Actualiza el juego para jugar en línea."}},
    {MP_T_CT_L_LATER, nullptr, {u"Please try again later.", u"Veuillez réessayer plus tard.", u"Inténtalo de nuevo más tarde."}},
    {MP_T_CT_L_CLOCK, nullptr, {u"Please set the date and time in the system settings.", u"Veuillez régler la date et l'heure de la console.", u"Configura la fecha y la hora en los ajustes del sistema."}},
    // m-files: the file screens and Safe Copies
    {MP_T_KS_CANT_READ, nullptr, {u"Unreadable", u"Abîmé", u"Ilegible"}},
    {MP_T_KS_CANT_READ_HINT, nullptr, {u"Can't read this file. Nothing was changed.", u"Fichier illisible. Rien n'a été modifié.", u"No se puede leer. No se ha cambiado nada."}},
    {MP_T_KS_SP_CANT_READ_HINT, nullptr, {u"Can't read this file, so it can't be used.", u"Illisible : ce fichier est inutilisable.", u"No se puede leer, así que no se puede usar."}},
    {MP_T_KS_SLOTS_CANT_READ, nullptr, {u"Some files can't be read. Nothing changed.", u"Fichiers illisibles : rien n'a été modifié.", u"Hay archivos ilegibles. Sin cambios."}},
    {MP_T_KS_NO_ROOM_COPY, nullptr, {u"None of your Team Files could be read,\nso there is nowhere to put a copy.\nNothing was changed.", u"Aucun de vos fichiers d'équipe n'est\nlisible : la copie n'a nulle part où aller.\nRien n'a été modifié.", u"No se pudo leer ningún archivo de equipo,\nasí que no hay dónde poner la copia.\nNo se ha cambiado nada."}},
    {MP_T_KS_FILE_NOT_READY, nullptr, {u"This file couldn't be read, so nothing\nwas written. Nothing was changed.", u"Ce fichier est illisible : rien n'a été\nécrit. Rien n'a été modifié.", u"No se pudo leer este archivo, así que no\nse escribió nada. No se ha cambiado nada."}},
    {MP_T_KS_PLAY_MINE, nullptr, {u"Play on your File " N1 u" (" S1 u" Power Stars)?\nYour game will be saved to it.\nA backup of the file is made first.", u"Jouer avec votre fichier " N1 u" (" S1 u" Étoiles) ?\nLa partie y sera sauvegardée.\nUne copie du fichier est faite avant.", u"¿Jugar con tu archivo " N1 u" (" S1 u" Estrellas)?\nLa partida se guardará en él.\nAntes se hace una copia del archivo."}},
    {MP_T_KS_SAVE_MINE, nullptr, {u"Save this progress to your File " N1 u"?\nThat file has " S1 u" Power Stars now; this has " S2 u".\nA backup of it is made first.", u"Sauvegarder cette progression dans votre\nfichier " N1 u" ? Il a " S1 u" Étoiles ; celle-ci " S2 u".\nUne copie en est faite avant.", u"¿Guardar este progreso en tu archivo " N1 u"?\nAhora tiene " S1 u" Estrellas; este, " S2 u".\nAntes se hace una copia."}},
    {MP_T_KS_SAVE_MINE_NOSTARS, nullptr, {u"Save this progress to your File " N1 u"?\nA backup of it is made first.", u"Sauvegarder cette progression dans votre\nfichier " N1 u" ? Une copie en est faite avant.", u"¿Guardar este progreso en tu archivo " N1 u"?\nAntes se hace una copia."}},
    {MP_T_KS_KEEP_TITLE, nullptr, {u"Safe Copies", u"Copies de sécurité", u"Copias de seguridad"}},
    {MP_T_KS_KEEP_ROW, nullptr, {u"Safe Copies", u"Copies de sécurité", u"Copias de seguridad"}},
    {MP_T_KS_KEEP_MAKE, nullptr, {u"Make a Copy", u"Faire une copie", u"Hacer una copia"}},
    {MP_T_KS_KEEP_BACK, nullptr, {u"Bring Back a Copy", u"Récupérer une copie", u"Recuperar una copia"}},
    {MP_T_KS_KEEP_H_MAKE, nullptr, {u"Saves your Team Files and Friend Code.", u"Garde vos fichiers d'équipe et code ami.", u"Guarda tus archivos de equipo y tu código."}},
    {MP_T_KS_KEEP_H_BACK, nullptr, {u"Go back to a copy you or the game made.", u"Revenez à une copie de vous ou du jeu.", u"Vuelve a una copia tuya o del juego."}},
    {MP_T_KS_KEEP_LIST_TITLE, nullptr, {u"Bring Back a Copy", u"Récupérer une copie", u"Recuperar una copia"}},
    {MP_T_KS_KEEP_MADE, nullptr, {u"Saved a copy of your multiplayer data.\nIt includes your Friend Code too.\nFor a friend, use Make a Copy to Share.", u"Copie de vos données multijoueur\nenregistrée, code ami compris.\nPour un ami : Faire une copie à partager.", u"Copia de tus datos multijugador guardada.\nIncluye también tu código de amigo.\nPara un amigo, usa la copia para compartir."}},
    {MP_T_KS_KEEP_NOTHING, nullptr, {u"You have no multiplayer data to copy yet.", u"Vous n'avez pas encore de données\nmultijoueur à copier.", u"Aún no tienes datos multijugador\nque copiar."}},
    {MP_T_KS_KEEP_NO_SPACE, nullptr, {u"There isn't enough room on the memory card.\nNothing was changed.", u"Il n'y a pas assez de place sur\nla carte mémoire.\nRien n'a été modifié.", u"No hay espacio suficiente en la\ntarjeta de memoria.\nNo se ha cambiado nada."}},
    {MP_T_KS_KEEP_FAILED, nullptr, {u"The copy couldn't be made.\nNothing was changed.", u"La copie n'a pas pu être faite.\nRien n'a été modifié.", u"No se pudo hacer la copia.\nNo se ha cambiado nada."}},
    {MP_T_KS_KEEP_BUSY, nullptr, {u"Please wait a moment and try again.\nNothing was changed.", u"Veuillez patienter un instant et réessayer.\nRien n'a été modifié.", u"Espera un momento e inténtalo de nuevo.\nNo se ha cambiado nada."}},
    {MP_T_KS_KS_NOTICE, nullptr, {u"Your memory card is full, so your data could\nnot be copied before the update.\nEverything is still as it was.", u"Votre carte mémoire est pleine : vos\ndonnées n'ont pas pu être copiées avant\nla mise à jour. Tout est resté comme avant.", u"Tu tarjeta de memoria está llena, así\nque no se pudieron copiar tus datos\nantes de actualizar. Todo sigue como estaba."}},
    {MP_T_KS_BK_ACCOUNT, nullptr, {u"This copy has a Friend Code.\nUse it on this console?", u"Cette copie contient un code ami.\nL'utiliser sur cette console ?", u"Esta copia tiene un código de amigo.\n¿Usarlo en esta consola?"}},
    {MP_T_KS_BK_ROLLED, nullptr, {u"That didn't work, so nothing was changed.\nYour multiplayer data is as it was.", u"Cela n'a pas fonctionné : rien n'a été\nmodifié. Vos données multijoueur sont\ncomme avant.", u"No funcionó, así que no se cambió nada.\nTus datos multijugador siguen como estaban."}},
    {MP_T_KS_BK_BAD, nullptr, {u"This copy can't be used. It is damaged or\nnot from this game. Nothing was changed.", u"Cette copie est inutilisable : abîmée ou\nd'un autre jeu. Rien n'a été modifié.", u"No se puede usar esta copia: está dañada o\nno es de este juego. No se ha cambiado nada."}},
    {MP_T_KS_BK_NEWER, nullptr, {u"This copy was made by a newer game.\nPlease update the game first.\nNothing was changed.", u"Cette copie vient d'une version\nplus récente. Veuillez d'abord mettre\nle jeu à jour. Rien n'a été modifié.", u"Esta copia es de una versión más reciente.\nPrimero actualiza el juego.\nNo se ha cambiado nada."}},
    {MP_T_KS_BK_NO_SPACE, nullptr, {u"There isn't enough room on the memory card.\nNothing was changed.", u"Il n'y a pas assez de place sur\nla carte mémoire.\nRien n'a été modifié.", u"No hay espacio suficiente en la\ntarjeta de memoria.\nNo se ha cambiado nada."}},
    {MP_T_KS_BK_NOTHING, nullptr, {u"There is nothing in this copy\nto bring back.", u"Il n'y a rien à récupérer dans\ncette copie.", u"No hay nada que recuperar\nen esta copia."}},
    // keepsafe API (share/keep/API.md): the new Safe Copies screens
    {MP_T_KS_NO_COPY, nullptr, {u"Your data could not be copied before\nthe update. Everything is as it was.\nIt is tried again at the next start.", u"Vos données n'ont pas pu être copiées\navant la mise à jour. Tout est comme avant.\nNouvel essai au prochain démarrage.", u"No se pudieron copiar tus datos antes\nde la actualización. Todo sigue igual.\nSe reintentará al iniciar de nuevo."}},
    {MP_T_KS_TOO_MANY, nullptr, {u"There are too many saved copies.\nNothing was changed.",
        u"Il y a trop de copies enregistrées.\nRien n'a été modifié.",
        u"Hay demasiadas copias guardadas.\nNo se ha cambiado nada."}},
    {MP_T_KS_KEEP_SHARE, nullptr, {u"Make a Copy to Share", u"Faire une copie à partager", u"Hacer una copia para compartir"}},
    {MP_T_KS_KEEP_H_SHARE, nullptr, {u"Like Make a Copy, but without a Friend Code.", u"Comme Faire une copie, sans code ami.", u"Como Hacer una copia, sin código de amigo."}},
    {MP_T_KS_MADE_SHARE, nullptr, {u"Saved a copy to share.\nIt has no Friend Code.\nIt's in the data folder: mp-backups", u"Copie à partager enregistrée.\nElle n'a pas de code ami.\nDossier de données : mp-backups", u"Copia para compartir guardada.\nNo incluye tu código de amigo.\nEn la carpeta de datos: mp-backups"}},
    {MP_T_KS_L_VERSION, nullptr, {u"Before " S1, u"Avant " S1, u"Antes de " S1}},
    {MP_T_KS_L_BEFORE, nullptr, {u"Auto copy", u"Copie auto", u"Copia auto"}},
    {MP_T_KS_L_MADE, nullptr, {u"Your copy", u"Votre copie", u"Tu copia"}},
    {MP_T_KS_H_HOLDS, nullptr, {u"Holds:", u"Contient :", u"Contiene:"}},
    {MP_T_KS_ROW_TEAMFILES, nullptr, {u"Team Files", u"Fichiers d'équipe", u"Archivos de equipo"}},
    {MP_T_KS_ROW_ACCOUNT, nullptr, {u"Friend Code", u"Code ami", u"Código de amigo"}},
    {MP_T_KS_ROW_SET_CONFIG, nullptr, {u"Name and Character",
        u"Nom et personnage",
        u"Nombre y personaje"}},
    {MP_T_KS_ROW_SET_FLOW, nullptr, {u"Last Choices", u"Derniers choix", u"Últimas elecciones"}},
    {MP_T_KS_ROW_SET_COOP, nullptr, {u"Who Played", u"Qui a joué", u"Quién jugó"}},
    {MP_T_KS_H_SLOT_NEW, nullptr, {u"Team File " N1 u" is empty here. This fills it.",
        u"Fichier d'équipe " N1 u" vide. Il sera rempli.",
        u"Archivo de equipo " N1 u" vacío. Se llenará."}},
    {MP_T_KS_H_SLOT_HERE, nullptr, {u"Here: " S1 u" Power Stars. It is saved first.", u"Ici : " S1 u" Étoiles. Il est d'abord enregistré.", u"Aquí: " S1 u" Power Stars. Se guarda antes."}},
    {MP_T_KS_H_ACCOUNT, nullptr, {u"Use the Friend Code from this copy.", u"Utiliser le code ami de cette copie.", u"Usar el código de amigo de esta copia."}},
    {MP_T_KS_Q_SLOT_REPLACE, nullptr, {u"Replace Team File " N1 u" with the one\nin this copy?\nWhat is there now is saved first.", u"Remplacer le fichier d'équipe " N1 u" par\ncelui de cette copie ?\nL'actuel est d'abord enregistré.", u"¿Reemplazar el archivo de equipo " N1 u" por\nel de esta copia?\nEl actual se guarda antes."}},
    {MP_T_KS_Q_TEAM_DIFFERENT, nullptr, {u"Team File " N1 u" here has progress the\ncopy doesn't have. Replace it anyway?\nWhat is there now is saved first.", u"Le fichier d'équipe " N1 u" a plus de progrès\nque la copie. Le remplacer quand même ?\nL'actuel est d'abord enregistré.", u"El archivo de equipo " N1 u" tiene progreso que\nla copia no tiene. ¿Reemplazarlo igual?\nEl actual se guarda antes."}},
    {MP_T_KS_Q_SET_CONFIG, nullptr, {u"Bring back your name and character\nfrom this copy?\nWhat is there now is saved first.",
        u"Récupérer votre nom et votre personnage\nde cette copie ?\nLes actuels sont d'abord enregistrés.",
        u"¿Recuperar tu nombre y tu personaje de\nesta copia?\nLos actuales se guardan antes."}},
    {MP_T_KS_Q_SET_FLOW, nullptr, {u"Bring back your remembered choices\nfrom this copy?\nWhat is there now is saved first.",
        u"Récupérer vos choix mémorisés de\ncette copie ?\nLes actuels sont d'abord enregistrés.",
        u"¿Recuperar tus elecciones guardadas de\nesta copia?\nLas actuales se guardan antes."}},
    {MP_T_KS_Q_SET_COOP, nullptr, {u"Bring back your Team File names from\nthis copy?\nWhat is there now is saved first.",
        u"Récupérer les noms de vos fichiers\nd'équipe de cette copie ?\nLes actuels sont d'abord enregistrés.",
        u"¿Recuperar los nombres de tus archivos\nde equipo de esta copia?\nLos actuales se guardan antes."}},
    {MP_T_KS_D_SLOT_BACK, nullptr, {u"Team File " N1 u" is back.\nWhat was there is in the list as\n\"Auto copy\".", u"Le fichier d'équipe " N1 u" est de retour.\nL'ancien est dans la liste :\n\"Copie auto\".", u"El archivo de equipo " N1 u" ha vuelto.\nEl anterior está en la lista como\n\"Copia auto\"."}},
    {MP_T_KS_D_SLOT_BACK_PLAIN, nullptr, {u"Team File " N1 u" is back.", u"Le fichier d'équipe " N1 u" est de retour.", u"El archivo de equipo " N1 u" ha vuelto."}},
    {MP_T_KS_D_SLOT_SAME, nullptr, {u"Team File " N1 u" is already the same.", u"Le fichier d'équipe " N1 u" est déjà identique.", u"El archivo de equipo " N1 u" ya es igual."}},
    {MP_T_KS_D_ACCOUNT_DONE, nullptr, {u"Done. Your Friend Code changed.", u"C'est fait.\nVotre code ami a changé.", u"Hecho. Tu código de amigo ha cambiado."}},
    {MP_T_KS_D_ACCOUNT_SAME, nullptr, {u"That is already your Friend Code.", u"C'est déjà votre code ami.", u"Ese ya es tu código de amigo."}},
    {MP_T_KS_D_SET_DONE, nullptr, {u"Done. It is back from the copy.\nWhat was there is in the list as\n\"Auto copy\".", u"C'est fait. C'est revenu de la copie.\nL'ancien est dans la liste :\n\"Copie auto\".", u"Hecho. Ha vuelto desde la copia.\nLo anterior está en la lista como\n\"Copia auto\"."}},
    {MP_T_G_WAITING, nullptr, {u"Waiting for friends...", u"En attente d'amis...", u"Esperando a tus amigos..."}},
    {MP_T_G_GETTING_CODE, nullptr, {u"Getting your code...", u"Obtention de votre code...", u"Obteniendo tu código..."}},
    {MP_T_G_TELL_CODE, nullptr, {u"Tell your friend this code.", u"Donnez ce code à votre ami.", u"Dile este código a tu amigo."}},
    {MP_T_G_CODE_LONG, nullptr, {u"Online play is busy, so this code is longer.\nShare it only with friends.", u"Jeu en ligne occupé : ce code est plus long.\nNe le donnez qu'à des amis.",
                                  u"Online va lento; este código es más largo.\nCompártelo solo con amigos."}},
    {MP_T_G_PRESS_START, nullptr, {u"Press \uE018 to start!", u"Appuyez sur \uE018 pour commencer !", u"¡Pulsa \uE018 para empezar!"}},
    {MP_T_G_WAIT_NAME, nullptr, {u"Waiting for " S1 u" to start...", u"En attente de " S1 u"...", u"Esperando a que " S1 u" empiece..."}},
    {MP_T_G_NOBODY, nullptr, {u"No one was found nearby.\nInvite a friend, or go back to play alone.", u"Personne n'est à proximité.\nInvitez un ami, ou revenez jouer seul.",
                              u"No se encontró a nadie cerca.\nInvita a un amigo o vuelve y juega solo."}},
    {MP_T_G_FULL, nullptr, {u"Your friend's game is full.", u"La partie de votre ami est complète.", u"La partida de tu amigo está llena."}},
    {MP_T_G_OTHER_VER, nullptr, {u"A nearby console has a different\nversion of the game.", u"Une console proche a une autre\nversion du jeu.", u"Una consola cercana tiene otra\nversión del juego."}},
    {MP_T_REJOIN_CLOSED, nullptr, {u"Your last game has closed.", u"Votre dernière partie est terminée.", u"Tu última partida ha terminado."}},
    {MP_T_G_JOIN_PLAYING, nullptr, {u"Play with " S1, u"Jouer avec " S1, u"Jugar con " S1}},   // wire: no Join (a game already started nearby)
    {MP_T_INVITE_FAR, nullptr, {u"Invite from Far Away", u"Inviter un ami au loin", u"Invitar desde lejos"}},
    {MP_T_JOIN_CODE, nullptr, {u"Enter a Friend's Code", u"Entrer un code d'ami", u"Escribir un código"}},   // wire: no Join (the friend far away reads a code aloud)
    {MP_T_SESSION_RUNS, nullptr, {u"The game keeps running.",
        u"La partie continue.",
        u"El juego sigue en marcha."}},
    {MP_T_GO_TO_Q, nullptr, {u"Who do you want to go to?", u"Vers qui voulez-vous aller ?", u"¿Con quién quieres ir?"}},
    {MP_T_PLAYERS_Q, nullptr, {u"Please choose a player.", u"Veuillez choisir un joueur.", u"Elige un jugador."}},
    {MP_T_PLAY_ANYONE, nullptr, {u"Play with Anyone", u"Jouer avec d'autres", u"Jugar con cualquiera"}},
    {MP_T_FALLS_Q, nullptr, {u"If someone falls,\nhow should they come back?", u"Si quelqu'un tombe,\ncomment doit-il revenir ?",
                             u"Si alguien cae,\n¿cómo debe volver?"}},
    {MP_T_FALLS_A_BUBBLE, nullptr, {u"In a Bubble", u"Dans une bulle", u"En una burbuja"}},
    {MP_T_FALLS_A_TRY, nullptr, {u"Try Again", u"Réessayer", u"Volver a intentarlo"}},
    {MP_T_CHANGE_FILE, nullptr, {u"Change File", u"Changer de fichier", u"Cambiar archivo"}},
    {MP_T_KB_DIRECT_CODE, nullptr, {u"Your friend's code", u"Code de votre ami", u"Código de tu amigo"}},
    // vitabtn: How to Play (the game's Co-Star manual names the Wii Remote): the controls of a session, with the Vita's glyphs
    {MP_T_DLG_HOWTO, "MP_HOWTO", {
        u"Hold \uE017 + a direction: quick message.\nD-pad down: say hello. \uE018: menu.\nHold \uE001 or touch: show your pointer.",
        u"Gardez \uE017 + direction : message rapide.\nD-pad bas : bonjour. \uE018 : menu.\nGardez \uE001 ou touchez : curseur visible.",
        u"Mant\u00E9n \uE017 + direcci\u00F3n: mensaje r\u00E1pido.\nD-pad abajo: saludar. \uE018: men\u00FA.\nMant\u00E9n \uE001 o toca: mostrar tu puntero."}},
    // menus4: How to Play's second page (a question; Yes opens Safe Copies)
    {MP_T_KS_HOWTO_SAFE, nullptr, {u"Keep My Multiplayer Data Safe\nMake a copy of your Team Files and Friend\nCode, or bring one back. Open it now?", u"Garder mes données multijoueur en sécurité\nFaire une copie de vos fichiers d'équipe et\nde votre code ami, ou en récupérer une ?", u"Mantener a salvo mis datos multijugador\nHaz una copia de tus archivos de equipo y\nde tu código, o recupera una. ¿Abrirlo?"}},
    {MP_T_DLG_REJOIN_GONE, "MP_REJOIN_GONE", {
        u"That game has ended.",
        u"Cette partie est terminée.",
        u"Esa partida ya terminó."}},
    // flow: MP_ERR_CLOCK (the online service refuses a console whose date and time are wrong)
    {MP_T_ERR_CLOCK, "MP_ERR_CLOCK", {u"Please set the date and time\nin the system settings.", u"Veuillez régler la date et l'heure\ndans les paramètres du système.",
                                      u"Ajusta la fecha y la hora\nen la configuración del sistema."}},
    // flow (savefix): the settings file is read only this run (MpSaveGame::settingsReadOnly): said once, plainly
    {MP_T_DLG_SETTINGS_RO, "MP_SETTINGS_RO", {u"Your settings can't be saved\nright now. Nothing else changes.", u"Vos réglages ne peuvent pas être\nenregistrés pour le moment.",
                                              u"Tus ajustes no se pueden guardar\nen este momento."}},
    // flow (one door): far away (a code read aloud; never an address, a port or a router)
    {MP_T_DLG_TELL_CODE, "MP_TELL_CODE", {u"Tell your friend this code.\n" S1, u"Donnez ce code à votre ami.\n" S1, u"Dile este código a tu amigo.\n" S1}},
    {MP_T_DLG_CODE_BAD, "MP_CODE_BAD", {u"That code doesn't look right.\nPlease check it and try again.",
        u"Ce code ne semble pas correct.\nVérifiez-le et réessayez.",
        u"Ese código no parece correcto.\nRevísalo e inténtalo otra vez."}},
    {MP_T_DLG_CODE_EXPIRED, "MP_CODE_EXPIRED", {u"That code isn't open any more.\nPlease ask your friend for a new one.",
        u"Ce code n'est plus valable.\nDemandez-en un nouveau à votre ami.",
        u"Ese código ya no está activo.\nPídele uno nuevo a tu amigo."}},
    {MP_T_DLG_CANT_REACH, "MP_CANT_REACH", {u"We couldn't reach your friend's game.\nCheck the code and try again.\nOr give your friend a code instead.",
        u"La partie de votre ami est injoignable.\nVérifiez le code et réessayez.\nOu donnez-lui plutôt un code.",
        u"No pudimos llegar a la partida de tu amigo.\nRevisa el código e inténtalo otra vez.\nO dale tú un código a tu amigo."}},
    // release3: a toast (no window name): shown once per boot when the online service says a newer version exists
    {MP_T_UPDATE_AVAILABLE, nullptr, {u"A newer version of the game is available.", u"Une version plus récente du jeu est disponible.",
                                      u"Hay una versión más reciente del juego."}},
    // conntest: offered after a failed join or host (the error window first)
    {MP_T_DLG_CT_OFFER, "MP_CT_OFFER", {u"That didn't work.\nSee who you can play with?", u"Cela n'a pas marché.\nVoir avec qui vous pouvez jouer ?", u"Eso no ha funcionado.\n¿Ver con quién puedes jugar?"}},
    // release3: Save a report (the file is report.txt in the game's data folder)
    {MP_T_G_REACHING, nullptr, {u"Reaching your friend...",
        u"Connexion à votre ami...",
        u"Buscando a tu amigo..."}},
    {MP_T_ERR_DROPPED, "MP_ERR_DROPPED", {u"The connection was lost.\nYou can go back in from the Multiplayer\nmenu for two minutes.",
        u"La connexion a été perdue.\nVous pouvez revenir depuis le menu\nMultijoueur pendant deux minutes.",
        u"Se perdió la conexión.\nPuedes volver a entrar desde el menú\nMultijugador durante dos minutos."}},
    {MP_T_DLG_DATA_MISMATCH, "MP_DATA_MISMATCH", {u"Your game didn't match your friend's, so\nyou left. Nothing on your console was\nchanged.", u"Votre jeu ne correspondait pas à celui de\nvotre ami : vous avez quitté la partie.\nRien n'a été modifié sur votre console.", u"Tu juego no coincidía con el de tu amigo,\nasí que saliste de la partida. No se ha\ncambiado nada en tu consola."}},
    {MP_T_WAIT_NAME, nullptr, {u"Waiting for " S1 u"...", u"En attente de " S1 u"...", u"Esperando a " S1 u"..."}},
    {MP_T_REFRESH, nullptr, {u"Refresh", u"Actualiser", u"Actualizar"}},
    {MP_T_DELETE_ACCOUNT, nullptr, {u"Delete My Online Account", u"Supprimer mon compte en ligne", u"Borrar mi cuenta en línea"}},
    {MP_T_DESC_DELETE_ACCOUNT, nullptr, {u"Removes your online account and friends list.", u"Supprime votre compte en ligne et votre liste d'amis.", u"Quita tu cuenta en línea y tu lista de amigos."}},
    {MP_T_DLG_DELETE_ACCOUNT, "MP_DELETE_ACCOUNT", {u"Delete your online account and\nfriends list? This can't be undone.", u"Supprimer votre compte en ligne et\nvotre liste d'amis ? C'est définitif.", u"¿Borrar tu cuenta en línea y tu\nlista de amigos? No se puede deshacer."}},
    {MP_T_ACCOUNT_DELETED, nullptr, {u"Your online account was deleted.", u"Votre compte en ligne a été supprimé.", u"Tu cuenta en línea fue borrada."}},
    {MP_T_RACE_IDLE_LEFT, nullptr, {u"No one chose, so you left the race.", u"Personne n'a choisi : vous avez quitté la course.", u"Nadie eligió, así que saliste de la carrera."}},
    {MP_T_RECONNECT_LONG, nullptr, {u"Still trying to reconnect...",
        u"Toujours en train de se reconnecter...",
        u"Sigo intentando reconectar..."}},
    // ---------------------------------------------------------------- beta diagnostics: Having Trouble? / Check Connection / Send a Problem Report
    {MP_T_RP_HELP, nullptr, {u"Having Trouble?",
        u"Un souci ?",
        u"¿Problemas?"}},
    {MP_T_RP_DESC_HELP, nullptr, {u"Check your connection or send a report.",
        u"Testez votre connexion ou envoyez un rapport.",
        u"Comprueba tu conexión o envía un informe."}},
    {MP_T_RP_CHECK, nullptr, {u"Check Connection",
        u"Tester la connexion",
        u"Comprobar conexión"}},
    {MP_T_RP_DESC_CHECK, nullptr, {u"See if this console can play online.",
        u"Voir si cette console peut jouer en ligne.",
        u"Mira si esta consola puede jugar en línea."}},
    {MP_T_RP_SEND, nullptr, {u"Send a Problem Report",
        u"Envoyer un rapport de problème",
        u"Enviar informe de problema"}},
    {MP_T_RP_DESC_SEND, nullptr, {u"Tell us what went wrong.",
        u"Dites-nous ce qui n'a pas marché.",
        u"Cuéntanos qué falló."}},
    {MP_T_RP_INFO1, nullptr, {u"A report sends your online name and\nthe game's log from recent play, with\nconnection details removed.",
        u"Un rapport envoie votre nom en ligne et\nle journal du jeu de vos dernières parties,\nsans les détails de connexion.",
        u"Un informe envía tu nombre en línea y\nel registro del juego de tus últimas\npartidas, sin datos de conexión."}},
    {MP_T_RP_INFO2, nullptr, {u"It also sends your last connection check\nand the numbers of your last online game.\nNo saves. No passwords.",
        u"Aussi votre dernier test de connexion et\nles chiffres de votre dernière partie.\nAucune sauvegarde, aucun mot de passe.",
        u"También envía tu última prueba de conexión\ny los números de tu última partida.\nSin partidas guardadas ni contraseñas."}},
    {MP_T_RP_ASK, nullptr, {u"Send this report?",
        u"Envoyer ce rapport ?",
        u"¿Enviar este informe?"}},
    {MP_T_RP_ASK_NOTE, nullptr, {u"Send this report with your note?",
        u"Envoyer ce rapport avec votre note ?",
        u"¿Enviar este informe con tu nota?"}},
    {MP_T_RP_B_SEND, nullptr, {u"Send",
        u"Envoyer",
        u"Enviar"}},
    {MP_T_RP_B_NOTE, nullptr, {u"Add a Note",
        u"Ajouter une note",
        u"Añadir nota"}},
    {MP_T_RP_B_CANCEL, nullptr, {u"Cancel",
        u"Annuler",
        u"Cancelar"}},
    {MP_T_RP_B_INFO, nullptr, {u"What's Sent",
        u"Contenu envoyé",
        u"Qué se envía"}},
    {MP_T_RP_KB_NOTE, nullptr, {u"Your note (200 letters)",
        u"Votre note (200 lettres)",
        u"Tu nota (200 letras)"}},
    {MP_T_RP_SENDING, nullptr, {u"Sending your report...",
        u"Envoi du rapport...",
        u"Enviando informe..."}},
    {MP_T_RP_CHECKING, nullptr, {u"Checking your connection...",
        u"Test de la connexion...",
        u"Comprobando conexión..."}},
    {MP_T_RP_THANKS, nullptr, {u"Thanks! Report " S1 u"\nTell us this number if you write to us.",
        u"Merci ! Rapport " S1 u"\nCitez ce numéro si vous nous écrivez.",
        u"¡Gracias! Informe " S1 u"\nDinos este número si nos escribes."}},
    {MP_T_RP_E_REJECT, nullptr, {u"The service did not accept\nthe report.",
        u"Le service n'a pas accepté\nle rapport.",
        u"El servicio no aceptó\nel informe."}},
    {MP_T_RP_E_BIG, nullptr, {u"The report is too big to send.",
        u"Le rapport est trop gros pour être envoyé.",
        u"El informe es demasiado grande."}},
    {MP_T_RP_E_LIMIT, nullptr, {u"You have sent enough reports today.\nPlease try later.",
        u"Vous avez envoyé assez de rapports\naujourd'hui. Réessayez plus tard.",
        u"Ya enviaste suficientes informes hoy.\nInténtalo más tarde."}},
    {MP_T_RP_E_BUSY, nullptr, {u"The service is busy.\nPlease try later.",
        u"Le service est occupé.\nRéessayez plus tard.",
        u"El servicio está ocupado.\nInténtalo más tarde."}},
    {MP_T_RP_E_OFFLINE, nullptr, {u"No connection.\nCheck your Wi-Fi.",
        u"Pas de connexion.\nVérifiez le Wi-Fi.",
        u"Sin conexión.\nRevisa el Wi-Fi."}},
    {MP_T_RP_E_TIMEOUT, nullptr, {u"The service did not answer.\nPlease try later.",
        u"Le service n'a pas répondu.\nRéessayez plus tard.",
        u"El servicio no respondió.\nInténtalo más tarde."}},
    {MP_T_RP_E_FAIL, nullptr, {u"The report could not be sent.\nPlease try later.",
        u"Le rapport n'a pas pu être envoyé.\nRéessayez plus tard.",
        u"No se pudo enviar el informe.\nInténtalo más tarde."}},
    {MP_T_RP_AFTER_ERR, nullptr, {u"Something went wrong last time.\nSend a report?",
        u"Un problème est survenu la dernière fois.\nEnvoyer un rapport ?",
        u"Algo salió mal la última vez.\n¿Enviar un informe?"}},
    {MP_T_RP_CK_DIRECT_OK, nullptr, {u"Direct play: should work",
        u"Jeu direct : devrait marcher",
        u"Juego directo: debería funcionar"}},
    {MP_T_RP_CK_DIRECT_RELAY, nullptr, {u"Direct play: may need the backup route",
        u"Jeu direct : la voie de secours peut aider",
        u"Juego directo: puede fallar"}},
    {MP_T_RP_CK_DIRECT_BLOCKED, nullptr, {u"Direct play: blocked",
        u"Jeu direct : bloqué",
        u"Juego directo: bloqueado"}},
    {MP_T_RP_CK_RELAY_OK, nullptr, {u"Backup route: works",
        u"Voie de secours : OK",
        u"Ruta de apoyo: funciona"}},
    {MP_T_RP_CK_RELAY_FAIL, nullptr, {u"Backup route: failed",
        u"Voie de secours : échec",
        u"Ruta de apoyo: falló"}},
    {MP_T_RP_CK_RELAY_SKIP, nullptr, {u"Backup route: not checked",
        u"Voie de secours : non testée",
        u"Ruta de apoyo: no comprobada"}},
    {MP_T_ERR_ROOM_STARTED, "MP_ERR_ROOM_STARTED", {u"That game has already begun.\nAsk your friend to invite you again.",
        u"Cette partie a déjà commencé.\nDemandez à votre ami de vous réinviter.",
        u"Esa partida ya empezó.\nPídele a tu amigo que te invite de nuevo."}},
    // settings (native in-game Settings, 14 Nov): see terms.md (Settings, Picture, Smooth, Detailed, Safe Saves, Less Motion, Change Buttons, Keep Editing)
    {MP_T_SET_ENTRY, nullptr, {u"Settings",
        u"Réglages",
        u"Ajustes"}},
    {MP_T_SET_BAND, nullptr, {u"What would you like to change?",
        u"Que voulez-vous changer ?",
        u"¿Qué quieres cambiar?"}},
    {MP_T_SET_L_PICTURE, nullptr, {u"Picture",
        u"Image",
        u"Imagen"}},
    {MP_T_SET_L_CAMERA, nullptr, {u"Camera",
        u"Caméra",
        u"Cámara"}},
    {MP_T_SET_L_ZOOM, nullptr, {u"Camera Zoom",
        u"Zoom de la caméra",
        u"Zoom de cámara"}},
    {MP_T_SET_L_RSTICK, nullptr, {u"Right Stick",
        u"Stick droit",
        u"Stick derecho"}},
    {MP_T_SET_L_INVERT, nullptr, {u"Invert Camera",
        u"Caméra inversée",
        u"Cámara invertida"}},
    {MP_T_SET_L_LANGUAGE, nullptr, {u"Language",
        u"Langue",
        u"Idioma"}},
    {MP_T_SET_L_CONTROLS, nullptr, {u"Controls",
        u"Commandes",
        u"Controles"}},
    {MP_T_SET_L_MORE, nullptr, {u"More",
        u"Plus",
        u"Más"}},
    {MP_T_SET_L_MOTION, nullptr, {u"Less Motion",
        u"Mouvement réduit",
        u"Menos movimiento"}},
    {MP_T_SET_L_REACH, nullptr, {u"Star Bit Reach",
        u"Portée des Morceaux d'Étoile",
        u"Alcance de Fragmentos"}},
    {MP_T_SET_L_RESET, nullptr, {u"Put Everything Back",
        u"Tout remettre",
        u"Restablecer todo"}},
    {MP_T_SET_L_TILT, nullptr, {u"Tilt",
        u"Inclinaison",
        u"Inclinación"}},
    {MP_T_SET_L_TILT_POWER, nullptr, {u"Tilt Strength",
        u"Force d'inclinaison",
        u"Fuerza de inclinación"}},
    {MP_T_SET_L_POINTER, nullptr, {u"Pointer",
        u"Curseur",
        u"Puntero"}},
    {MP_T_SET_L_REAR, nullptr, {u"Rear Touch",
        u"Toucher arrière",
        u"Toque trasero"}},
    {MP_T_SET_L_TAPJUMP, nullptr, {u"Tap to Jump",
        u"Toucher pour sauter",
        u"Tocar para saltar"}},
    {MP_T_SET_L_BUTTONS, nullptr, {u"Change Buttons",
        u"Changer les boutons",
        u"Cambiar botones"}},
    {MP_T_SET_V_SMOOTH, nullptr, {u"Smooth Motion",
        u"Mouvement fluide",
        u"Movimiento fluido"}},
    {MP_T_SET_V_DETAILED, nullptr, {u"Full Detail",
        u"Tous les détails",
        u"Todos los detalles"}},
    {MP_T_SET_V_SYSTEM_LANG, nullptr, {u"System Language",
        u"Langue de la console",
        u"Idioma de la consola"}},
    {MP_T_SET_V_CONSOLE, nullptr, {u"Console",
        u"Console",
        u"Consola"}},
    {MP_T_SET_V_LEFT_STICK, nullptr, {u"Left Stick",
        u"Stick gauche",
        u"Stick izquierdo"}},
    {MP_T_SET_V_CAMERA, nullptr, {u"Camera",
        u"Caméra",
        u"Cámara"}},
    {MP_T_SET_V_POINTER, nullptr, {u"Pointer",
        u"Curseur",
        u"Puntero"}},
    {MP_T_SET_V_NORMAL, nullptr, {u"Normal",
        u"Normal",
        u"Normal"}},
    {MP_T_SET_V_NONE, nullptr, {u"None",
        u"Aucun",
        u"Ninguno"}},
    {MP_T_SET_V_STEADY, nullptr, {u"Steady",
        u"Stable",
        u"Estable"}},
    {MP_T_SET_V_QUICK, nullptr, {u"Quick",
        u"Rapide",
        u"Rápido"}},
    // modeux32: the Picture sentences name both modes; the _NEW pair (the choice differs from what is running) says the restart. Words: "frames" stays out (the owner's rule). French and Spanish not checked by a native speaker.
    {MP_T_SET_D_PICTURE_SMOOTH, nullptr, {u"Performance: aims for 60 a second, smoother motion.",
        u"Performance : vise 60 par seconde, mouvement fluide.",
        u"Rendimiento: busca 60 por segundo, más fluido."}},
    {MP_T_SET_D_PICTURE_DETAILED, nullptr, {u"Quality: a steady 30 a second, sharpest picture.",
        u"Qualité : 30 par seconde stables, image la plus nette.",
        u"Calidad: 30 por segundo estables, imagen más nítida."}},
    {MP_T_SET_D_PICTURE_SMOOTH_NEW, nullptr, {u"Performance: aims for 60. Save and Start Again to use it.",
        u"Performance : vise 60. Sauvegarder et relancer pour l'utiliser.",
        u"Rendimiento: busca 60. Guardar y abrir otra vez para usarlo."}},
    {MP_T_SET_D_PICTURE_DETAILED_NEW, nullptr, {u"Quality: a steady 30. Save and Start Again to use it.",
        u"Qualité : 30 stables. Sauvegarder et relancer pour l'utiliser.",
        u"Calidad: 30 estables. Guardar y abrir otra vez para usarlo."}},
    {MP_T_SET_D_CAMERA, nullptr, {u"Change how the camera zooms and turns.",
        u"Changez le zoom et les mouvements de la caméra.",
        u"Cambia cómo se acerca y gira la cámara."}},
    {MP_T_SET_D_ZOOM, nullptr, {u"Pulls the camera in or out. 1.0 is how the game was made.",
        u"Rapproche ou éloigne la caméra. 1,0 : comme le jeu.",
        u"Acerca o aleja la cámara. 1.0 es como el juego."}},
    {MP_T_SET_D_RSTICK_CAMERA, nullptr, {u"The right stick turns the camera.",
        u"Le stick droit tourne la caméra.",
        u"El stick derecho gira la cámara."}},
    {MP_T_SET_D_RSTICK_POINTER, nullptr, {u"The right stick moves the pointer.",
        u"Le stick droit déplace le curseur.",
        u"El stick derecho mueve el puntero."}},
    {MP_T_SET_D_INVERT_ON, nullptr, {u"Pushing the stick up turns the camera down.",
        u"Pousser le stick vers le haut baisse la caméra.",
        u"Empujar el stick arriba baja la cámara."}},
    {MP_T_SET_D_INVERT_OFF, nullptr, {u"Pushing the stick up turns the camera up.",
        u"Pousser le stick vers le haut lève la caméra.",
        u"Empujar el stick arriba sube la cámara."}},
    {MP_T_SET_D_LANG_OWN, nullptr, {u"The game uses your console's language: " S1 u".",
        u"Le jeu utilise la langue de votre console : " S1 u".",
        u"El juego usa el idioma de tu consola: " S1 u"."}},
    {MP_T_SET_D_LANG_NAMED, nullptr, {u"The game's words are shown in this language.",
        u"Les textes du jeu sont dans cette langue.",
        u"Los textos del juego salen en este idioma."}},
    {MP_T_SET_D_REACH, nullptr, {u"Picks up Star Bits from further away. 1 is the game's own.",
        u"Ramasse les Morceaux d'Étoile de plus loin. 1 : comme le jeu.",
        u"Recoge Fragmentos desde más lejos. 1 es como el juego."}},
    {MP_T_SET_D_RESET, nullptr, {u"Puts every setting and button back to standard.",
        u"Remet tous les réglages et boutons par défaut.",
        u"Devuelve todos los ajustes y botones a lo estándar."}},
    {MP_T_SET_D_SESSION, nullptr, {u"Change this when you're not playing together.",
        u"Changez ceci quand vous ne jouez pas ensemble.",
        u"Cambia esto cuando no jueguen juntos."}},
    {MP_T_SET_D_NOT_RACE, nullptr, {u"Not during a race.",
        u"Pas pendant une course.",
        u"No durante una carrera."}},
    {MP_T_SET_D_RESTART_NEXT, nullptr, {u"Starts the next time the game starts.",
        u"Prend effet au prochain démarrage du jeu.",
        u"Empieza la próxima vez que se abra el juego."}},
    {MP_T_SET_D_HOLD, nullptr, {u"Set back to standard so the game could start.",
        u"Remis par défaut pour pouvoir démarrer le jeu.",
        u"Vuelto a lo estándar para poder abrir el juego."}},
    {MP_T_SET_D_SESSION_MENU, nullptr, {u"Change your camera, tilt and a few more things.",
        u"Changez la caméra, l'inclinaison et d'autres choses.",
        u"Cambia tu cámara, la inclinación y alguna cosa más."}},
    {MP_T_SET_D_MOTION_ON, nullptr, {u"Less camera shake and fewer bright flashes.",
        u"Moins de secousses de la caméra et de flashs.",
        u"Menos temblor de cámara y menos destellos."}},
    {MP_T_SET_D_MOTION_OFF, nullptr, {u"Camera shake and flashes as the game made them.",
        u"Secousses et flashs comme dans le jeu d'origine.",
        u"Temblor y destellos como los hizo el juego."}},
    {MP_T_SET_D_TILT_CONSOLE, nullptr, {u"Tilt the console to steer the Star Ball.",
        u"Inclinez la console pour diriger la Boule Étoile.",
        u"Inclina la consola para mover la Bola Estelar."}},
    {MP_T_SET_D_TILT_STICK, nullptr, {u"Use the left stick to steer the Star Ball.",
        u"Utilisez le stick gauche pour diriger la Boule Étoile.",
        u"Usa el stick izquierdo para mover la Bola Estelar."}},
    {MP_T_SET_D_POINTER_STEADY, nullptr, {u"The pointer moves as fast as you push the stick.",
        u"Le curseur va aussi vite que vous poussez le stick.",
        u"El puntero va tan rápido como empujas el stick."}},
    {MP_T_SET_D_POINTER_QUICK, nullptr, {u"Small pushes aim finely; keep pushing and it speeds up.",
        u"Petites poussées : visée fine ; en continuant, il accélère.",
        u"Empujes leves apuntan fino; si sigues, acelera."}},
    {MP_T_SET_D_REAR_ON, nullptr, {u"Touch the back of the console to aim the pointer.",
        u"Touchez l'arrière de la console pour viser.",
        u"Toca la parte trasera de la consola para apuntar."}},
    {MP_T_SET_D_REAR_OFF, nullptr, {u"The back of the console does nothing.",
        u"L'arrière de la console ne fait rien.",
        u"La parte trasera de la consola no hace nada."}},
    {MP_T_SET_D_TAPJUMP_ON, nullptr, {u"A short tap on the screen jumps.",
        u"Un petit toucher sur l'écran fait sauter.",
        u"Un toque corto en la pantalla salta."}},
    {MP_T_SET_D_TAPJUMP_OFF, nullptr, {u"Touching the screen only moves the pointer.",
        u"Toucher l'écran ne fait que bouger le curseur.",
        u"Tocar la pantalla solo mueve el puntero."}},
    {MP_T_SET_D_CONTROLS, nullptr, {u"Change tilt, the pointer, touch and the buttons.",
        u"Changez l'inclinaison, le curseur, le toucher et les boutons.",
        u"Cambia la inclinación, el puntero, el toque y los botones."}},
    {MP_T_SET_D_MORE, nullptr, {u"Language, less motion and Star Bit reach.",
        u"Langue, mouvement réduit et portée des Morceaux d'Étoile.",
        u"Idioma, menos movimiento y alcance de Fragmentos."}},
    {MP_T_SET_D_TILT_POWER, nullptr, {u"Bigger numbers steer more from a small tilt. 5 is normal.",
        u"Plus le chiffre est grand, plus on dirige. 5 : normal.",
        u"Un número mayor mueve más con poca inclinación. 5 es normal."}},
    {MP_T_SET_D_BUTTONS, nullptr, {u"Choose a button for each move.",
        u"Choisissez un bouton pour chaque action.",
        u"Elige un botón para cada acción."}},
    {MP_T_SET_D_CAPTURE, nullptr, {u"Press the button you want for " S1 u".",
        u"Appuyez sur le bouton pour " S1 u".",
        u"Oprime el botón para " S1 u"."}},
    {MP_T_SET_D_CAPTURE_TIMEOUT, nullptr, {u"No button pressed. Choose one from the list.",
        u"Aucun bouton pressé. Choisissez dans la liste.",
        u"No oprimiste ningún botón. Elige de la lista."}},
    {MP_T_SET_TITLE_BTN_CONSOLE, nullptr, {u"Buttons for this console",
        u"Boutons de cette console",
        u"Botones de esta consola"}},
    {MP_T_SET_TITLE_BTN_PAD, nullptr, {u"Buttons for the controller",
        u"Boutons de la manette",
        u"Botones del mando"}},
    {MP_T_SET_D_BUTTONS_NEXT, nullptr, {u"New buttons start the next time the game starts.",
        u"Les nouveaux boutons valent au prochain démarrage.",
        u"Los botones nuevos valen la próxima vez que se abra el juego."}},
    {MP_T_SET_D_TEST_FILE, nullptr, {u"A test file on this console sets this one.",
        u"Un fichier de test de cette console règle ceci.",
        u"Un archivo de prueba de esta consola lo decide."}},
    {MP_T_SET_LANG_ENGLISH, nullptr, {u"English",
        u"English",
        u"English"}},
    {MP_T_SET_LANG_FRENCH, nullptr, {u"Français",
        u"Français",
        u"Français"}},
    {MP_T_SET_LANG_SPANISH, nullptr, {u"Español",
        u"Español",
        u"Español"}},
    {MP_T_SET_LANG_GERMAN, nullptr, {u"Deutsch",
        u"Deutsch",
        u"Deutsch"}},
    {MP_T_SET_LANG_ITALIAN, nullptr, {u"Italiano",
        u"Italiano",
        u"Italiano"}},
    {MP_T_SET_M_JUMP, nullptr, {u"Jump",
        u"Sauter",
        u"Saltar"}},
    {MP_T_SET_M_STAR_BITS, nullptr, {u"Shoot Star Bits",
        u"Lancer des Morceaux d'Étoile",
        u"Lanzar Fragmentos"}},
    {MP_T_SET_M_SPIN, nullptr, {u"Spin",
        u"Tourbillonner",
        u"Girar"}},
    {MP_T_SET_M_SPIN2, nullptr, {u"Spin (Second Button)",
        u"Tourbillonner (2e bouton)",
        u"Girar (2.º botón)"}},
    {MP_T_SET_M_CROUCH, nullptr, {u"Crouch",
        u"S'accroupir",
        u"Agacharse"}},
    {MP_T_SET_M_CAMERA, nullptr, {u"Center Camera",
        u"Recentrer la caméra",
        u"Centrar la cámara"}},
    {MP_T_SET_M_PAUSE, nullptr, {u"Pause",
        u"Pause",
        u"Pausa"}},
    {MP_T_SET_M_AIM, nullptr, {u"Reset Aim",
        u"Réinitialiser la visée",
        u"Reiniciar la puntería"}},
    {MP_T_SET_M_CAM_UP, nullptr, {u"Camera Up",
        u"Caméra vers le haut",
        u"Cámara arriba"}},
    {MP_T_SET_M_CAM_DOWN, nullptr, {u"Camera Down",
        u"Caméra vers le bas",
        u"Cámara abajo"}},
    {MP_T_SET_M_CAM_LEFT, nullptr, {u"Camera Left",
        u"Caméra vers la gauche",
        u"Cámara a la izquierda"}},
    {MP_T_SET_M_CAM_RIGHT, nullptr, {u"Camera Right",
        u"Caméra vers la droite",
        u"Cámara a la derecha"}},
    {MP_T_SET_MD_JUMP, nullptr, {u"Jump, and pick things in menus.",
        u"Sauter et choisir dans les menus.",
        u"Salta y elige en los menús."}},
    {MP_T_SET_MD_STAR_BITS, nullptr, {u"Shoots Star Bits. Also goes Back in menus.",
        u"Lance des Morceaux d'Étoile. Sert aussi à revenir.",
        u"Lanza Fragmentos. También vuelve atrás en los menús."}},
    {MP_T_SET_MD_SPIN, nullptr, {u"Spin to attack and to use Launch Stars.",
        u"Tourbillonnez pour attaquer et vous lancer.",
        u"Gira para atacar y para lanzarte."}},
    {MP_T_SET_MD_SPIN2, nullptr, {u"Another button that spins too.",
        u"Un autre bouton qui fait tourbillonner aussi.",
        u"Otro botón que también gira."}},
    {MP_T_SET_MD_CROUCH, nullptr, {u"Crouch low, or crouch and jump to flip.",
        u"Accroupissez-vous, ou sautez accroupi pour un salto.",
        u"Agáchate, o agáchate y salta para dar una voltereta."}},
    {MP_T_SET_MD_CAMERA, nullptr, {u"Puts the camera behind you. Hold to look around.",
        u"Remet la caméra derrière vous. Maintenez pour regarder.",
        u"Pone la cámara detrás de ti. Mantén para mirar."}},
    {MP_T_SET_MD_PAUSE, nullptr, {u"Pauses the game.",
        u"Met le jeu en pause.",
        u"Pone el juego en pausa."}},
    {MP_T_SET_MD_AIM, nullptr, {u"Puts the pointer and the tilt back in the middle.",
        u"Remet le curseur et l'inclinaison au centre.",
        u"Vuelve a centrar el puntero y la inclinación."}},
    {MP_T_SET_MD_CAM_UP, nullptr, {u"Turns the camera up.",
        u"Dirige la caméra vers le haut.",
        u"Mueve la cámara hacia arriba."}},
    {MP_T_SET_MD_CAM_DOWN, nullptr, {u"Turns the camera down.",
        u"Dirige la caméra vers le bas.",
        u"Mueve la cámara hacia abajo."}},
    {MP_T_SET_MD_CAM_LEFT, nullptr, {u"Turns the camera left.",
        u"Dirige la caméra vers la gauche.",
        u"Mueve la cámara hacia la izquierda."}},
    {MP_T_SET_MD_CAM_RIGHT, nullptr, {u"Turns the camera right.",
        u"Dirige la caméra vers la droite.",
        u"Mueve la cámara hacia la derecha."}},
    {MP_T_SET_B_R, nullptr, {u"R",
        u"R",
        u"R"}},
    {MP_T_SET_B_L2, nullptr, {u"L2",
        u"L2",
        u"L2"}},
    {MP_T_SET_B_R2, nullptr, {u"R2",
        u"R2",
        u"R2"}},
    {MP_T_SET_B_L3, nullptr, {u"L3",
        u"L3",
        u"L3"}},
    {MP_T_SET_B_R3, nullptr, {u"R3",
        u"R3",
        u"R3"}},
    {MP_T_SET_B_UP, nullptr, {u"D-pad Up",
        u"D-pad haut",
        u"D-pad arriba"}},
    {MP_T_SET_B_DOWN, nullptr, {u"D-pad Down",
        u"D-pad bas",
        u"D-pad abajo"}},
    {MP_T_SET_B_LEFT, nullptr, {u"D-pad Left",
        u"D-pad gauche",
        u"D-pad izquierda"}},
    {MP_T_SET_B_RIGHT, nullptr, {u"D-pad Right",
        u"D-pad droite",
        u"D-pad derecha"}},
    {MP_T_SET_B_CROSS, nullptr, {u"Cross",
        u"Croix",
        u"Cruz"}},
    {MP_T_SET_B_CIRCLE, nullptr, {u"Circle",
        u"Rond",
        u"Círculo"}},
    {MP_T_SET_B_SQUARE, nullptr, {u"Square",
        u"Carré",
        u"Cuadrado"}},
    {MP_T_SET_B_TRIANGLE, nullptr, {u"Triangle",
        u"Triangle",
        u"Triángulo"}},
    {MP_T_SET_B_L, nullptr, {u"L",
        u"L",
        u"L"}},
    {MP_T_SET_B_SELECT, nullptr, {u"Select",
        u"Select",
        u"Select"}},
    {MP_T_SET_B_START, nullptr, {u"Start",
        u"Start",
        u"Start"}},
    {MP_T_SET_LEAVE_Q, nullptr, {u"Save your changes?",
        u"Voulez-vous sauvegarder vos changements ?",
        u"¿Quieres guardar tus cambios?"}},
    {MP_T_SET_SAVE, nullptr, {u"Save",
        u"Sauvegarder",
        u"Guardar"}},
    {MP_T_SET_SAVE_RESTART, nullptr, {u"Save and Start Again",
        u"Sauvegarder et relancer",
        u"Guardar y abrir otra vez"}},
    {MP_T_SET_DONT_SAVE, nullptr, {u"Don't Save",
        u"Ne pas sauvegarder",
        u"No guardar"}},
    {MP_T_SET_KEEP_EDITING, nullptr, {u"Keep Editing",
        u"Modifier encore",
        u"Seguir cambiando"}},
    {MP_T_SET_RESET, nullptr, {u"Reset",
        u"Rétablir",
        u"Restablecer"}},
    {MP_T_SET_TOAST_SAVED, nullptr, {u"Settings saved.",
        u"Réglages sauvegardés.",
        u"Ajustes guardados."}},
    {MP_T_SET_TOAST_SAVED_LATER, nullptr, {u"Settings saved. Picture and language change next time.",
        u"Réglages sauvegardés. Image et langue changent plus tard.",
        u"Ajustes guardados. Imagen e idioma cambian después."}},
    {MP_T_SET_TOAST_RECOVERED, nullptr, {u"Settings were set back to standard for this start.",
        u"Les réglages standard sont utilisés pour ce démarrage.",
        u"Se usaron los ajustes estándar en este inicio."}},
    {MP_T_SET_TOAST_UNREADABLE, nullptr, {u"Your settings couldn't be read, so the standard ones are used.",
        u"Vos réglages n'ont pas pu être lus : les standard sont utilisés.",
        u"No se pudieron leer tus ajustes, así que se usan los estándar."}},
    {MP_T_SET_OPEN, nullptr, {u"Open", u"Ouvrir", u"Abrir"}},
    {MP_T_SET_L_REPORT, nullptr, {u"Send a Problem Report", u"Envoyer un rapport de problème", u"Enviar un informe de problema"}},
    {MP_T_SET_D_REPORT, nullptr, {u"Send the game's log to help fix bugs.", u"Envoie le journal du jeu pour aider à corriger les bogues.", u"Envía el registro del juego para ayudar a corregir errores."}},
    {MP_T_SET_CAP_LIST, nullptr, {u"Touch: list", u"Toucher : liste", u"Tocar: lista"}},
    {MP_T_SET_TOAST_NO_ROOM, nullptr, {u"Settings can't open here. Try from the Galaxy menu.",
        u"Les réglages ne s'ouvrent pas ici. Essayez depuis le menu Galaxie.",
        u"Los ajustes no se abren aquí. Prueba desde el menú Galaxia."}},
    {MP_T_SET_TOAST_HOLD, nullptr, {u"Picture and language were set back to standard so the game could start.",
        u"L'image et la langue ont été remises par défaut pour démarrer le jeu.",
        u"La imagen y el idioma volvieron a lo estándar para abrir el juego."}},
    {MP_T_SET_DLG_RESTART_FAIL, "MP_SET_RESTART_FAIL", {u"Saved. Please close the game and\nstart it again.",
        u"Sauvegardé. Veuillez fermer le jeu\net le relancer.",
        u"Guardado. Cierra el juego\ny ábrelo otra vez."}},
    {MP_T_SET_DLG_CONFLICT, "MP_SET_CONFLICT", {S1 u" is used for\n" S2 u".\nSwap them?",
        S1 u" est déjà utilisé pour\n" S2 u".\nLes échanger ?",
        S1 u" se usa para\n" S2 u".\n¿Cambiarlos?"}},
    {MP_T_SET_DLG_LOST, "MP_SET_LOST", {u"That would leave a move without a button.\nPlease choose another one.",
        u"Une action resterait sans bouton.\nChoisissez-en un autre.",
        u"Una acción se quedaría sin botón.\nElige otro."}},
    {MP_T_SET_DLG_UNAVAILABLE, "MP_SET_UNAVAILABLE", {u"That button isn't available here.\nPlease choose another one.",
        u"Ce bouton n'est pas disponible ici.\nChoisissez-en un autre.",
        u"Ese botón no está disponible aquí.\nElige otro."}},
    {MP_T_SET_DLG_RESET, "MP_SET_RESET", {u"Put back the standard buttons?",
        u"Remettre les boutons standard ?",
        u"¿Volver a los botones estándar?"}},
    {MP_T_SET_DLG_RESET_ALL, "MP_SET_RESET_ALL", {u"Put every setting and button\nback to standard?",
        u"Remettre tous les réglages et les\nboutons par défaut ?",
        u"¿Devolver todos los ajustes y\nbotones a lo estándar?"}},
    // setnat31: the 60 Assist row (French and Spanish not checked by a native speaker)
    {MP_T_SET_L_ASSIST, nullptr, {u"60 Assist",
        u"Assistance 60",
        u"Asistencia 60"}},
    {MP_T_SET_D_ASSIST_ON, nullptr, {u"Only where 60 can't hold: every other picture estimated.",
        u"Seulement sans 60 stable : une image sur deux est estimée.",
        u"Solo si no se mantiene 60: se estima una imagen de cada dos."}},
    // setapply31 (French and Spanish not checked by a native speaker)
    {MP_T_SET_D_RESTART_ASK, nullptr, {u"The game starts again to use this; you're asked first.",
        u"Le jeu repart pour l'appliquer ; on vous demande avant.",
        u"El juego vuelve a empezar para usarlo; se te pregunta antes."}},
    {MP_T_SET_DLG_RESTART_PAUSE, "MP_SET_RESTART_PAUSE", {u"Start the game again now?\nYou leave this level. Progress\nsince your last save is lost.",
        u"Relancer le jeu maintenant ?\nVous quittez ce niveau. Ce qui suit\nla dernière sauvegarde est perdu.",
        u"¿Volver a empezar el juego ahora?\nSales de este nivel. Se pierde lo\nhecho desde que guardaste."}},
    // setfix31b: "Save and Start Again" while a save is still being written (French and Spanish not checked by a native speaker)
    {MP_T_SET_RESTART_WAIT, nullptr, {u"Finishing saving... the game starts again as soon as it's done.",
        u"Fin de la sauvegarde... le jeu repart dès que c'est fini.",
        u"Terminando de guardar... el juego vuelve a empezar al acabar."}},
    {MP_T_SET_RESTART_STUCK, nullptr, {u"Saving is taking too long. Your settings are already saved.",
        u"La sauvegarde est trop longue. Vos réglages sont déjà sauvés.",
        u"Guardar tarda demasiado. Tus ajustes ya están guardados."}},
    {MP_T_SET_RESTART_STUCK_LATER, nullptr, {u"Saving is taking too long. Your settings apply next start.",
        u"La sauvegarde est trop longue. Réglages au prochain lancement.",
        u"Guardar tarda demasiado. Los ajustes se usarán la próxima vez."}},
    {MP_T_SET_START_NOW, nullptr, {u"Start Again Now",
        u"Relancer maintenant",
        u"Empezar ahora"}},
    {MP_T_SET_LATER, nullptr, {u"Later",
        u"Plus tard",
        u"Más tarde"}},
    {MP_T_SET_D_ASSIST_OFF, nullptr, {u"Only real pictures: no shimmer, but busy spots look less smooth.",
        u"Que de vraies images : rien ne scintille, mais moins fluide.",
        u"Solo imágenes reales: sin parpadeo, pero menos fluido."}},
    // even60: the "Allow in-between rates (50)" row (French and Spanish not checked by a native speaker)
    {MP_T_SET_L_ALLOW50, nullptr, {u"Allow in-between rates (50)",
        u"Autoriser les cadences intermédiaires (50)",
        u"Permitir ritmos intermedios (50)"}},
    {MP_T_SET_D_ALLOW50_ON, nullptr, {u"Uneven rates like 50 allowed: smooth in quiet places, jumpy in busy ones.",
        u"Cadences inégales (50) permises : saccades possibles.",
        u"Se permiten ritmos irregulares como 50: puede ir a saltos."}},
    {MP_T_SET_D_ALLOW50_OFF, nullptr, {u"Always an even 60 or 30; busy places drop to 30 or 60/30.",
        u"Toujours 60 ou 30 réguliers ; les zones chargées passent à 30 ou 60/30.",
        u"Siempre 60 o 30 estables; las zonas cargadas bajan a 30 o 60/30."}},
};

const char16_t kYes[LANG_COUNT][6] = {u"Yes", u"Oui", u"Sí", u"Ja", u"Sì"};
const char16_t kNo[LANG_COUNT][6] = {u"No", u"Non", u"No", u"Nein", u"No"};

int sLang = LANG_EN;
const Entry* sIndex[MP_T_COUNT];
bool sIndexed = false;

void BuildIndex() {
    if (sIndexed) return;
    for (const Entry& e : kEntries)
        if (e.id < MP_T_COUNT) sIndex[e.id] = &e;
    sIndexed = true;
}

// Dynamic window texts.
char16_t sDynamic[2][320];

}  // namespace

int languageIndex(const char* prefix) {
    if (!prefix) return LANG_EN;
    const size_t n = strlen(prefix);
    auto ends = [&](const char* w) {
        const size_t k = strlen(w);
        return n >= k && strcmp(prefix + n - k, w) == 0;
    };
    if (ends("French")) return LANG_FR;
    if (ends("Spanish")) return LANG_ES;
    if (ends("German")) return LANG_DE;
    if (ends("Italian")) return LANG_IT;
    return LANG_EN;
}
void setLanguage(int lang) { sLang = lang >= 0 && lang < LANG_COUNT ? lang : LANG_EN; }
int language() { return sLang; }
// (weak: a test that links only this file still builds; MpMessagesExtra.cpp has the real one)
__attribute__((weak)) const char16_t* extraText(int, int) { return nullptr; }

const char16_t* raw(MpText id) {
    BuildIndex();
    if (id >= MP_T_COUNT || !sIndex[id]) return u"";
    const char16_t* t = sLang >= LANG_DE ? extraText(id, sLang) : sIndex[id]->text[sLang];   // German/Italian: MpMessagesExtra.cpp
    return t ? t : sIndex[id]->text[LANG_EN];
}

const char* dialogName(MpText id) {
    BuildIndex();
    return id < MP_T_COUNT && sIndex[id] ? sIndex[id]->name : nullptr;
}

MpText dialogByName(const char* name, size_t len) {
    for (const Entry& e : kEntries)
        if (e.name && strlen(e.name) == len && memcmp(e.name, name, len) == 0) return e.id;
    return MP_T_NONE;
}

MpText errorText(int err) {
    // (MpUiError order: NONE, NO_WIFI, SERVICE_DOWN, UPDATE_REQUIRED, ROOM_NOT_FOUND, ROOM_FULL, DIFFERENT_VERSION, TIMEOUT,
    // HOST_LEFT, KICKED, NAT, NAME_TAKEN, BAD_NAME, RATE_LIMITED, INTERNAL)
    static const MpText kMap[] = {MP_T_ERR_INTERNAL, MP_T_ERR_NO_WIFI, MP_T_ERR_SERVICE_DOWN, MP_T_ERR_UPDATE, MP_T_ERR_ROOM_NOT_FOUND,
                                  MP_T_ERR_ROOM_FULL, MP_T_ERR_VERSION, MP_T_ERR_TIMEOUT, MP_T_ERR_HOST_LEFT, MP_T_ERR_KICKED,
                                  MP_T_ERR_NAT, MP_T_ERR_NAME_TAKEN, MP_T_ERR_BAD_NAME, MP_T_ERR_RATE, MP_T_ERR_INTERNAL,
                                  MP_T_ERR_CLOCK};   // (MP_ERR_CLOCK, appended after INTERNAL)
    return err >= 0 && err < static_cast<int>(sizeof kMap / sizeof kMap[0]) ? kMap[err] : MP_T_ERR_INTERNAL;
}

MpText serviceText(const char* code) {
    if (!code || !*code) return MP_T_NONE;
    static const struct { const char* code; MpText text; } kMap[] = {
        {"banned", MP_T_ERR_BANNED},             {"friends_full", MP_T_ERR_FRIENDS_FULL},   {"not_friends", MP_T_ERR_NOT_FRIENDS},
        {"player_not_found", MP_T_ERR_NO_PLAYER}, {"own_code", MP_T_ERR_OWN_CODE},           {"not_member", MP_T_ERR_NOT_MEMBER},
        {"daily_cap", MP_T_ERR_DAILY},            {"rate_limited", MP_T_ERR_RATE},           {"name_taken", MP_T_ERR_NAME_TAKEN},
        {"bad_name", MP_T_ERR_BAD_NAME},          {"version_mismatch", MP_T_ERR_VERSION},    {"game_mismatch", MP_T_ERR_VERSION},
        {"update_required", MP_T_ERR_UPDATE},     {"upgrade_required", MP_T_ERR_UPDATE},     {"room_full", MP_T_ERR_ROOM_FULL},
        {"room_started", MP_T_ERR_ROOM_STARTED},     {"room_not_found", MP_T_ERR_ROOM_NOT_FOUND}, {"room_closed", MP_T_ERR_ROOM_NOT_FOUND},
        {"room_expired", MP_T_ERR_ROOM_NOT_FOUND}, {"service_disabled", MP_T_ERR_SERVICE_DOWN},
        {"service_busy", MP_T_ERR_BUSY},          {"over_budget", MP_T_ERR_BUSY},
    };
    for (const auto& e : kMap)
        if (strcmp(e.code, code) == 0) return e.text;
    return MP_T_NONE;
}

int utf8To16(char16_t* out, int cap, const char* in, int inLen) {
    if (cap <= 0) return 0;
    int n = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in ? in : "");
    const unsigned char* end = inLen >= 0 ? p + inLen : nullptr;
    while (n < cap - 1 && (end ? p < end : true) && *p) {
        uint32_t c = *p, need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) need = 1, c &= 0x1F;
        else if ((c & 0xF0) == 0xE0) need = 2, c &= 0x0F;
        else if ((c & 0xF8) == 0xF0) need = 3, c &= 0x07;
        else { out[n++] = 0xFFFD; ++p; continue; }
        ++p;
        bool ok = true;
        for (uint32_t i = 0; i < need; i++) {
            if ((end && p >= end) || (*p & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (*p++ & 0x3F);
        }
        if (!ok || c > 0xFFFF || (c >= 0xD800 && c < 0xE000) || (c >= 0xE000 && c < 0xE100) || c < 0x20) c = 0xFFFD;  // (no markup from outside)
        out[n++] = static_cast<char16_t>(c);
    }
    out[n] = 0;
    return n;
}

int formatTime(char16_t* out, int cap, uint32_t turns) {
    // m:ss.cc, at most 99:59.99
    uint64_t cs = static_cast<uint64_t>(turns) * 100 / 60;
    if (cs > 99ull * 6000 + 5999) cs = 99ull * 6000 + 5999;
    const unsigned m = static_cast<unsigned>(cs / 6000), s = static_cast<unsigned>(cs / 100 % 60), c = static_cast<unsigned>(cs % 100);
    char16_t buf[12];
    int n = 0;
    if (m >= 10) buf[n++] = static_cast<char16_t>(u'0' + m / 10);
    buf[n++] = static_cast<char16_t>(u'0' + m % 10);
    buf[n++] = u':';
    buf[n++] = static_cast<char16_t>(u'0' + s / 10);
    buf[n++] = static_cast<char16_t>(u'0' + s % 10);
    buf[n++] = u'.';
    buf[n++] = static_cast<char16_t>(u'0' + c / 10);
    buf[n++] = static_cast<char16_t>(u'0' + c % 10);
    int k = 0;
    for (; k < n && k < cap - 1; k++) out[k] = buf[k];
    if (cap > 0) out[k] = 0;
    return k;
}

int formatRaw(char16_t* out, int cap, const char16_t* text, const Args& a) {
    if (cap <= 0) return 0;
    int n = 0;
    for (const char16_t* p = text ? text : u""; *p && n < cap - 1; ++p) {
        if (*p == 1 || *p == 2) {
            n += utf8To16(out + n, cap - n, *p == 1 ? a.str : a.str2);
        } else if (*p == 3) {
            char16_t digits[12];
            int k = 0;
            long long v = a.num;
            const bool neg = v < 0;
            if (neg) v = -v;
            do { digits[k++] = static_cast<char16_t>(u'0' + v % 10); v /= 10; } while (v && k < 11);
            if (neg && n < cap - 1) out[n++] = u'-';
            while (k > 0 && n < cap - 1) out[n++] = digits[--k];
        } else if (*p == 4) {
            n += formatTime(out + n, cap - n, a.turns);
        } else {
            out[n++] = *p;
        }
    }
    out[n] = 0;
    return n;
}

int format(char16_t* out, int cap, MpText id, const Args& a) { return formatRaw(out, cap, raw(id), a); }

int toWindowText(char16_t* out, int cap, const char16_t* text, bool leadingTag) {
    if (cap <= 0) return 0;
    int n = 0;
    auto put = [&](char16_t c) { if (n < cap - 1) out[n++] = c; };
    if (leadingTag && cap > 4) put(0x1A), put(0x0106), put(0x0003);
    for (const char16_t* p = text ? text : u""; *p; ++p) {
        if (isPicChar(*p)) {
            if (n + 3 >= cap - 1) break;
            put(0x1A), put(0x0306), put(static_cast<char16_t>(*p - 0xE000));
        } else if (*p >= 0x20 || *p == u'\n') {
            put(*p);
        }
    }
    out[n] = 0;
    return n;
}

void setDynamic(int slot, MpText id, const Args& a) {
    if (slot < 0 || slot > 1) return;
    char16_t tmp[320];
    format(tmp, 320, id, a);
    toWindowText(sDynamic[slot], 320, tmp, true);
}
void setDynamicLines(int slot, const MpText* ids, int count) {   // conntest: the Connection Test's verdict lines
    if (slot < 0 || slot > 1) return;
    char16_t tmp[320];
    int n = 0;
    for (int i = 0; i < count && i < 3; i++) {
        if (i > 0 && n < 319) tmp[n++] = u'\n';
        n += format(tmp + n, 320 - n, ids[i], Args());
    }
    tmp[n < 320 ? n : 319] = 0;
    toWindowText(sDynamic[slot], 320, tmp, true);
}
void setDynamicLinesArgs(int slot, const MpText* ids, const Args* args, int count) {   // beta diagnostics
    if (slot < 0 || slot > 1) return;
    char16_t tmp[320];
    int n = 0;
    for (int i = 0; i < count && i < 3; i++) {
        if (i > 0 && n < 319) tmp[n++] = u'\n';
        n += format(tmp + n, 320 - n, ids[i], args ? args[i] : Args());
    }
    tmp[n < 320 ? n : 319] = 0;
    toWindowText(sDynamic[slot], 320, tmp, true);
}
const char16_t* dynamicText(int slot) { return slot >= 0 && slot <= 1 ? sDynamic[slot] : u""; }

// touches: the host-rules card
// uxfix #7: two lines (who chooses, and what a fall costs or how the race is won). Bumping is always on (not a rule to tell), and the card is
// no longer a question ("Show this next time?" was one decision too many); it is an OK card.
void setDynamicRules(int slot, const Rules& r) {
    if (slot < 0 || slot > 1) return;
    const bool race = r.mode == 1;
    Args h;
    h.str = r.host ? r.host : "";
    char16_t a[120], b[120], tmp[320];
    format(a, 120, race ? MP_T_RULES_HOST_R : MP_T_RULES_HOST_T, h);
    if (race) format(b, 120, MP_T_RULES_RACE, Args());
    else format(b, 120, r.falls == 1 ? MP_T_RULES_FALL_TRY : MP_T_RULES_FALL_BUBBLE, Args());
    int k = 0;
    for (const char16_t* p = a; *p && k < 300; ++p) tmp[k++] = *p;
    tmp[k++] = u'\n';
    for (const char16_t* p = b; *p && k < 316; ++p) tmp[k++] = *p;
    tmp[k] = 0;
    toWindowText(sDynamic[slot], 320, tmp, true);
}

int formatRulesToast(char16_t* out, int cap, const Rules& r, bool changed) {
    char16_t parts[100];
    char16_t piece[40];
    int k = 0;
    auto add = [&](MpText id) {
        if (k > 0 && k < 95) { parts[k++] = u','; parts[k++] = u' '; }
        const int n = format(piece, 40, id, Args());
        for (int i = 0; i < n && k < 98; ++i) parts[k++] = piece[i];
        parts[k] = 0;
    };
    parts[0] = 0;
    add(r.mode == 1 ? MP_T_STAR_RACE : MP_T_PLAY_TOGETHER);
    if (r.mode != 1) {
        add(r.falls == 1 ? MP_T_RULES_P_TRY : MP_T_RULES_P_BUBBLES);   // (uxfix #7: bumping is always on: not told)
    }
    // the parts as lower case words after the mode's name: "Play Together, bubbles"
    char utf8[3 * 100 + 1];
    int n = 0;
    for (int i = 0; parts[i] && n < 3 * 98; ++i) {
        const char16_t ch = parts[i];
        if (ch < 0x80) utf8[n++] = static_cast<char>(ch);
        else if (ch < 0x800) { utf8[n++] = static_cast<char>(0xC0 | (ch >> 6)); utf8[n++] = static_cast<char>(0x80 | (ch & 0x3F)); }
        else { utf8[n++] = static_cast<char>(0xE0 | (ch >> 12)); utf8[n++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F)); utf8[n++] = static_cast<char>(0x80 | (ch & 0x3F)); }
    }
    utf8[n] = 0;
    Args a;
    a.str = r.host ? r.host : "";
    a.str2 = utf8;
    return format(out, cap, changed ? MP_T_RULES_CHANGED : MP_T_RULES_TOAST, a);
}

const char16_t* findDialog(const char* id) {
    if (!id || id[0] != 'M' || id[1] != 'P' || id[2] != '_') return nullptr;
    size_t len = strlen(id);
    int which = 0;  // 0 text, 1 yes, 2 no
    if (len > 4 && strcmp(id + len - 4, "_Yes") == 0) which = 1, len -= 4;
    else if (len > 3 && strcmp(id + len - 3, "_No") == 0) which = 2, len -= 3;
    if (which == 1) return kYes[sLang];
    if (which == 2) return kNo[sLang];
    if (len == 7 && memcmp(id, "MP_DYN", 6) == 0 && (id[6] == '0' || id[6] == '1')) return sDynamic[id[6] - '0'];
    const MpText t = dialogByName(id, len);
    if (t == MP_T_NONE) return nullptr;
    // a fixed text: converted once per language into a small ring (the text boxes copy what they get)
    static char16_t ring[4][320];
    static int next = 0;
    char16_t* out = ring[next];
    next = (next + 1) & 3;
    Args none;
    char16_t tmp[320];
    format(tmp, 320, t, none);
    toWindowText(out, 320, tmp, true);
    return out;
}

}  // namespace MpMsgText

namespace MpMsgText {
namespace {
const char* const kGalaxies[] = {
    // Terrace
    "EggStarGalaxy", "HoneyBeeKingdomGalaxy", "SurfingLv1Galaxy", "FlipPanelExGalaxy", "TriLegLv1Galaxy", "BeltConveyerExGalaxy",
    // Fountain
    "StarDustGalaxy", "BattleShipGalaxy", "TamakoroExLv1Galaxy", "BreakDownPlanetGalaxy", "KoopaBattleVs1Galaxy", "CocoonExGalaxy",
    // Kitchen
    "HeavenlyBeachGalaxy", "PhantomGalaxy", "CubeBubbleExLv1Galaxy", "OceanFloaterLandGalaxy", "KoopaJrShipLv1Galaxy", "TearDropGalaxy",
    // Bedroom
    "CosmosGardenGalaxy", "IceVolcanoGalaxy", "SandClockGalaxy", "HoneyBeeExGalaxy", "KoopaBattleVs2Galaxy", "FishTunnelGalaxy",
    // Engine Room
    "ReverseKingdomGalaxy", "OceanRingGalaxy", "FactoryGalaxy", "SkullSharkGalaxy", "FloaterOtaKingGalaxy", "TransformationExGalaxy",
    // Garage
    "OceanPhantomCaveGalaxy", "CannonFleetGalaxy", "HellProminenceGalaxy", "DarkRoomGalaxy", "KoopaBattleVs3Galaxy", "SnowCapsuleGalaxy",
    // elsewhere
    "HeavensDoorGalaxy", "SurfingLv2Galaxy", "TamakoroExLv2Galaxy", "CubeBubbleExLv2Galaxy", "TeresaMario2DGalaxy", "PeachCastleFinalGalaxy",
};
constexpr int kGalaxyCount = sizeof kGalaxies / sizeof kGalaxies[0];
}  // namespace

int galaxyCount() { return kGalaxyCount; }
const char* galaxyStage(int index) { return index >= 0 && index < kGalaxyCount ? kGalaxies[index] : nullptr; }
int galaxyIndexOf(const char* stage) {
    if (!stage) return -1;
    for (int i = 0; i < kGalaxyCount; i++)
        if (strcmp(stage, kGalaxies[i]) == 0) return i;
    return -1;
}

}  // namespace MpMsgText
