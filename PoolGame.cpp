#include <emscripten.h>
#include <emscripten/html5.h>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <ctime>

using namespace std;

const double scale = 0.85;				// shrink everything to 85% so it fits on screen nicely
const double screen_w = 1800 * scale;				// total canvas width after scaling
const double screen_h = 900 * scale;				// total canvas height after scaling
const double table_x = 450 * scale;				// where the green table starts (left panel takes the rest)
const double ball_radius = 20 * scale;				// every ball on the table is this big
const double hole_radius = 30 * scale;				// pockets are slightly bigger than balls so they catch them
const double cue_power = 3 * scale;				// multiplier that controls how hard the cue hits
const double border_w = 10 * scale;				// thickness of the wooden rail around the table
const double baulk_w = 2 * scale;				// thickness of the baulk line drawn across the table

const double friction = 0.99;				// each frame a ball keeps 99% of its speed — slows down gradually
const double min_speed = 0.5;				// below this speed we just freeze the ball so it doesn't crawl forever
const double bounce = 1.0;				// 1.0 = no energy loss on collisions, perfectly elastic

const string white = "#ffffff";				// cue ball color
const string black = "#000000";				// black ball + hole color
const string red = "#ff0000";				// red ball color
const string blue = "#0000ff";				// blue ball color
const string yellow = "#ffff00";				// yellow ball color
const string orange = "#ffc800";				// orange ball color
const string pink = "#ffafaf";				// the dangerous ball — pot it last or you lose instantly
const string dark_gray = "#404040";				// left panel background color
const string light_gray = "#c0c0c0";				// faint aiming line color
const string dark_green = "#006400";				// dark green ball color
const string brown = "#8b4513";				// wooden rail color around the table
const string purple = "#800080";				// purple ball color
const string felt_green = "#35654d";				// the green cloth color of the table surface
const string none = "none";				// used when we want no fill or no stroke on a shape

EM_JS(void, drawRect,
      (double x, double y, double w, double h, const char *color), {
          Module._ctx.fillStyle = UTF8ToString(color);				// set the fill color
          Module._ctx.fillRect(x, y, w, h);				// paint the rectangle
      });

EM_JS(void, drawRectOutline,
      (double x, double y, double w, double h, const char *color, double lw), {
          Module._ctx.strokeStyle = UTF8ToString(color);				// outline color
          Module._ctx.lineWidth = lw;				// how thick the outline is
          Module._ctx.strokeRect(x, y, w, h);				// draw just the border, no fill
      });

EM_JS(void, drawCircle,
      (double cx, double cy, double r,
       const char *fillColor, const char *strokeColor, double strokeW),
      {
          const ctx = Module._ctx;
          ctx.beginPath();
          ctx.arc(cx, cy, r, 0, Math.PI * 2);				// trace a full circle path
          const f = UTF8ToString(fillColor);
          if (f !== "none")				// only fill if a real color was given
          {
              ctx.fillStyle = f;
              ctx.fill();
          }
          const s = UTF8ToString(strokeColor);
          if (s !== "none")				// only stroke if a real color was given
          {
              ctx.strokeStyle = s;
              ctx.lineWidth = strokeW;
              ctx.stroke();
          }
      });

EM_JS(void, drawLine,
      (double x1, double y1, double x2, double y2, const char *color, double lw), {
          const ctx = Module._ctx;
          ctx.strokeStyle = UTF8ToString(color);				// line color
          ctx.lineWidth = lw;				// line thickness
          ctx.beginPath();
          ctx.moveTo(x1, y1);				// start point
          ctx.lineTo(x2, y2);				// end point
          ctx.stroke();				// actually draw it
      });

EM_JS(void, drawArc,
      (double cx, double cy, double r,
       double startAngle, double endAngle, const char *color, double lw),
      {
          const ctx = Module._ctx;
          ctx.strokeStyle = UTF8ToString(color);				// arc outline color
          ctx.lineWidth = lw;
          ctx.beginPath();
          ctx.arc(cx, cy, r, startAngle, endAngle, false);				// draw the curved segment
          ctx.stroke();
      });

EM_JS(void, drawText,
      (const char *text, double x, double y, const char *color, int size, int bold), {
          const ctx = Module._ctx;
          ctx.font = (bold ? "bold " : "") + size + "px 'Rajdhani', Arial";				// build the font string
          ctx.fillStyle = UTF8ToString(color);
          ctx.textBaseline = "alphabetic";				// consistent vertical alignment
          ctx.fillText(UTF8ToString(text), x, y);
      });

EM_JS(double, measureText, (const char *text, int size, int bold), {
    const ctx = Module._ctx;
    ctx.font = (bold ? "bold " : "") + size + "px 'Rajdhani', Arial";				// match the font used when drawing
    return ctx.measureText(UTF8ToString(text)).width;				// return pixel width so we can center text
});

EM_JS(double, getscaleX, (), {
    const c = document.getElementById("gameCanvas");
    return c.width / c.getBoundingClientRect().width;				// how much the canvas is stretched horizontally
});

EM_JS(double, getscaleY, (), {
    const c = document.getElementById("gameCanvas");
    return c.height / c.getBoundingClientRect().height;				// how much the canvas is stretched vertically
});

EM_JS(double, getCanvasLeft, (), {
    return document.getElementById("gameCanvas").getBoundingClientRect().left;				// canvas left edge on screen
});

EM_JS(double, getCanvasTop, (), {
    return document.getElementById("gameCanvas").getBoundingClientRect().top;				// canvas top edge on screen
});

// ── file-handling data types ────────────────────────────────────────────────

struct ScoreEntry
{
    string name;
    int    score;
    bool operator<(const ScoreEntry &other) const { return score < other.score; }				// used by sortScores for descending comparison
};

static ScoreEntry  g_scoreData[5];                              // top-5 scores populated by fileGetCount
static int         g_scoreCount = 0;                            // number of valid entries in g_scoreData
static const char *SCORE_FILE = "pool9ball_scores.txt";         // file used to persist all high scores

// ── internal helpers ────────────────────────────────────────────────────────

static string trimStr(const string &s)                          // strip leading and trailing whitespace
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == string::npos) ? "" : s.substr(a, b - a + 1);
}

static void sortScores(ScoreEntry *arr, int n)                  // insertion sort — descending by score
{
    for (int i = 1; i < n; i++)
    {
        ScoreEntry key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] < key)             // shift entries lower than key to the right
        {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static ScoreEntry *readAllScores(int &count)                    // parse every data row from the score file; caller must delete[]
{
    int capacity = 16;
    count= 0;
    ScoreEntry *all = new ScoreEntry[capacity];

    ifstream fin(SCORE_FILE);
    if (!fin.is_open())
        return all;

    string line;
    while (getline(fin, line))
    {
        if (line.size() < 4 || line[0] != '|')                 // skip borders, headers, blank lines
            continue;
        if (line.find('#') == string::npos)                     // data rows always carry a '#' rank token
            continue;

        size_t p1 = line.find('|', 1);                         // find the separator after the rank column
        size_t p2 = line.find('|', p1 + 1);                    // find the separator after the name column
        if (p1 == string::npos || p2 == string::npos)
            continue;

        string nm = trimStr(line.substr(p1 + 1, p2 - p1 - 1)); // extract and clean the player name
        string sc = trimStr(line.substr(p2 + 1));               // extract and clean the score text
        if (!sc.empty() && sc.back() == '|')                    // strip the trailing '|' if present
            sc = trimStr(sc.substr(0, sc.size() - 1));

        if (nm.empty() || sc.empty())
            continue;
        try
        {
            if (count >= capacity)                              // grow the array when it fills up
            {
                capacity        *= 2;
                ScoreEntry *tmp  = new ScoreEntry[capacity];
                for (int i = 0; i < count; i++)
                    tmp[i] = all[i];
                delete[] all;
                all = tmp;
            }
            all[count].name  = nm;
            all[count].score = stoi(sc);
            count++;
        }
        catch (...) {}
    }
    fin.close();
    return all;
}

extern "C" void downloadScoreFile(const char *content);          // forward declaration — defined later as an EM_JS function

static void writeScoreFile(const ScoreEntry *all, int count)    // write all scores to disk in formatted table
{
    ofstream f(SCORE_FILE);
    if (!f.is_open())
        return;

    // ── column content widths (characters between the cell padding spaces) ──
    const int CW1 = 8;   // rank   column  e.g. "#1      "
    const int CW2 = 24;  // name   column  e.g. "Alice                   "
    const int CW3 = 8;   // score  column  e.g. "     350"

    // total line width = 2+CW1+3+CW2+3+CW3+2 = 50 chars (including outer '|' pair)
    const int INNER = (CW1 + 2) + 1 + (CW2 + 2) + 1 + (CW3 + 2); // = 48

    // sep   = column-divider     e.g. '=' (merged) or '+' (visible joint)
    auto hLine = [&](char edge, char fill, char sep)
    {
        f << edge << string(CW1 + 2, fill) << sep << string(CW2 + 2, fill) << sep << string(CW3 + 2, fill) << edge << "\n";
    };

    // ── lambda: print one data row ───────────────────────────────────────────
    auto dataRow = [&](const string &c1, const string &c2, const string &c3, bool scoreRight)
    {
        ostringstream row;
        row << "| " << left  << setw(CW1) << c1 << " | " << left  << setw(CW2) << c2 << " | ";
        if (scoreRight)
            row << right << setw(CW3) << c3;
        else
            row << left  << setw(CW3) << c3;
        row << " |";
        f << row.str() << "\n";
    };

    // ── timestamp ────────────────────────────────────────────────────────────
    time_t now = time(nullptr);
    char   ts[32] = {};
    strftime(ts, sizeof(ts), "%Y-%m-%d  %H:%M", localtime(&now));

    // ── centered title line ──────────────────────────────────────────────────
    const string TITLE = "POOL 9-BALL  HIGH SCORES";
    int lpad = (INNER - (int)TITLE.size()) / 2;
    int rpad = INNER - lpad - (int)TITLE.size();
    string titleLine = "|" + string(lpad, ' ') + TITLE + string(rpad, ' ') + "|";

    // ── footer line (entry count + timestamp) ───────────────────────────────
    ostringstream footerSS;
    footerSS << "  Entries: " << left << setw(4) << count
             << "  Updated: " << ts;
    string footerStr = footerSS.str();
    if ((int)footerStr.size() < INNER)                          // pad to full inner width
        footerStr += string(INNER - (int)footerStr.size(), ' ');
    string footerLine = "|" + footerStr + "|";

    // ── write the file ───────────────────────────────────────────────────────
    hLine('+', '=', '=');
    f << titleLine  << "\n";
    hLine('+', '=', '=');
    dataRow("Rank", "Player", "Score", false);                  // column headers (left-aligned)
    hLine('+', '-', '+');
    for (int i = 0; i < count; i++)                             // one row per score entry
        dataRow("#" + to_string(i + 1), all[i].name, to_string(all[i].score), true);
    hLine('+', '-', '+');
    f << footerLine << "\n";
    hLine('+', '=', '=');

    f.close();

    ifstream readBack(SCORE_FILE);				// re-open the file we just wrote so we can pass its content to JS
    ostringstream ss;
    ss << readBack.rdbuf();				// read every byte into a string stream
    readBack.close();
    downloadScoreFile(ss.str().c_str());				// hand the text to JS — browser will offer a Save dialog
}

// ── public file-handling functions ───────────────────────────────────────────

void fileSaveScore(const char *name, int score)
{
    int         oldCount = 0;
    ScoreEntry *all      = readAllScores(oldCount);             // load existing scores

    int         newCount  = oldCount + 1;
    ScoreEntry *extended  = new ScoreEntry[newCount];           // make room for the new entry
    for (int i = 0; i < oldCount; i++)
        extended[i] = all[i];
    delete[] all;

    extended[oldCount].name  = string(name);
    extended[oldCount].score = score;                           // add the new entry

    sortScores(extended, newCount);
    if (newCount > 100)
        newCount = 100;                                         // cap stored entries so the file stays manageable

    writeScoreFile(extended, newCount);                         // write back to storage
    delete[] extended;
}

int fileGetCount()
{
    int         count = 0;
    ScoreEntry *all   = readAllScores(count);
    sortScores(all, count);                                     // highest score first

    g_scoreCount = (count < 5) ? count : 5;                    // keep only the top 5
    for (int i = 0; i < g_scoreCount; i++)
        g_scoreData[i] = all[i];

    delete[] all;
    return g_scoreCount;
}

void fileGetName(int i, char *buf, int len)
{
    if (i < g_scoreCount)
        strncpy(buf, g_scoreData[i].name.c_str(), len - 1);    // copy name into C++ buffer
}

int fileGetScore(int i)
{
    if (i < g_scoreCount)
        return g_scoreData[i].score;                            // return the score at position i
    return 0;
}

EM_JS(void, playHit, (), {
    if (typeof playHit_js === 'function')
        playHit_js();				// call external JS helper if it exists
    var s = document.getElementById("hitSound");
    if (s)
    {
        s.currentTime = 0;				// rewind so it plays from the start every time
        s.play();
    }
});

EM_JS(void, playPot, (), {
    var s = document.getElementById("potSound");
    if (s)
    {
        s.currentTime = 0;				// rewind before playing
        s.play();
    }
});

EM_JS(void, startMusic, (), {
    var m = document.getElementById("bgMusic");
    if (m)
    {
        m.volume = 0.4;				// quiet enough not to overpower sound effects
        m.play();
    }
});

EM_JS(void, showGameOverButtons, (), {
    var btns = document.getElementById("gameOverBtns");
    if (btns)
        btns.style.display = "flex";				// reveal the Play Again / Exit buttons
});

EM_JS(void, downloadScoreFile, (const char *content), {
    var text = UTF8ToString(content);				// convert the C++ string to a JS string
    var blob = new Blob([text], { type: "text/plain" });				// wrap it in a plain-text blob
    var a = document.createElement("a");
    a.href = URL.createObjectURL(blob);				// create a temporary download URL
    a.download = "pool9ball_scores.txt";				// the file name the browser will save it as
    a.click();				// trigger the download automatically
});

EM_JS(void, playHitSound, (), {
    var snd = new Audio("hit.mp3");				// create a fresh audio instance each collision
    snd.volume = 0.5;
    snd.play();
});

class GameObject
{
private:
    double x;
    double y;

protected:
    GameObject(double startX, double startY)				// every object needs a starting position
    {
        x = startX;
        y = startY;
    }

public:
    virtual ~GameObject() {}

    double getX() { return x; }				// read-only access to x position
    double getY() { return y; }				// read-only access to y position
    void setX(double newX) { x = newX; }				// update x position
    void setY(double newY) { y = newY; }				// update y position

    virtual void draw() = 0;				// every subclass must know how to draw itself
};

class Ball : public GameObject
{
private:
    bool moving;
    bool gone;				// true once the ball has been potted — it leaves the game entirely
    string color;

public:
    double radius;
    double velocityX;				// pixels per second moving left/right
    double velocityY;				// pixels per second moving up/down

    Ball(double x, double y, double r, string c)
        : GameObject(x, y)
    {
        radius = r;
        color = c;
        velocityX = 0;				// start completely still
        velocityY = 0;
        moving = false;
        gone = false;
    }

    bool is_moving() { return moving; }
    void set_moving(bool v) { moving = v; }
    bool is_gone() { return gone; }				// check if this ball has already been potted
    void setGone(bool v) { gone = v; }
    string getColor() { return color; }

    void draw() override
    {
        if (gone)				// skip drawing — ball is already in the pocket
            return;
        drawCircle(getX(), getY(), radius, color.c_str(), white.c_str(), 1.0);				// filled circle with white outline
    }

    void resetTo(double x, double y)				// snap ball back to a spot and kill all movement
    {
        setX(x);
        setY(y);
        velocityX = 0;
        velocityY = 0;
        moving = false;
        gone = false;
    }
};

class CueBall : public Ball
{
private:
    double spawnX;				// the cue ball always returns here after a foul
    double spawnY;

public:
    CueBall(double x, double y, double r)
        : Ball(x, y, r, white)				// cue ball is always white
    {
        spawnX = x;
        spawnY = y;
    }

    double getSpawnX() { return spawnX; }
    double getSpawnY() { return spawnY; }

    void draw() override
    {
        Ball::draw();				// draw the white circle first
        if (is_gone())
            return;
        drawCircle(getX(), getY(), radius * 0.2,
                   "rgba(150,150,150,0.5)", none.c_str(), 0);				// subtle grey dot to make it look like a real cue ball
    }
};

class Hole : public GameObject
{
private:
    double r;

public:
    Hole(double x, double y, double radius)
        : GameObject(x, y)
    {
        r = radius;
    }

    double getRadius() { return r; }

    void draw() override
    {
        drawCircle(getX(), getY(), r, black.c_str(), none.c_str(), 0);				// solid black circle, no outline
    }
};

class PlayerScore
{
private:
    string name;
    int score;

public:
    PlayerScore()				// default constructor for empty entries
    {
        name = "";
        score = 0;
    }

    PlayerScore(string n, int s)
    {
        name = n;
        score = s;
    }

    string getName() { return name; }
    int getScore() { return score; }

    bool operator>(const PlayerScore &other) const { return score > other.score; }				// true when this player outranks the other

    string toString() { return name + " - " + to_string(score); }				// formatted for display on screen
};

class Physics
{
private:
    double tableX;
    double screenW;
    double screenH;
    double halfBorder;				// half the border thickness — used to find the exact inner wall edge

public:
    Physics(double tx, double sw, double sh, double bw)
    {
        tableX = tx;
        screenW = sw;
        screenH = sh;
        halfBorder = bw / 2.0;
    }

    void moveBall(Ball &ball, double dt)				// advance the ball one time step and apply friction
    {
        if (ball.is_gone())				// potted balls don't move
            return;

        ball.setX(ball.getX() + ball.velocityX * dt);				// move horizontally
        ball.setY(ball.getY() + ball.velocityY * dt);				// move vertically

        ball.velocityX = ball.velocityX * friction;				// bleed off a little speed each frame
        ball.velocityY = ball.velocityY * friction;

        double speed = sqrt(ball.velocityX * ball.velocityX + ball.velocityY * ball.velocityY);

        if (speed < min_speed)				// ball is barely moving — just stop it completely
        {
            ball.velocityX = 0;
            ball.velocityY = 0;
            ball.set_moving(false);
        }
        else
        {
            ball.set_moving(true);
        }
    }

    void wallBounce(Ball &ball)				// flip velocity when a ball hits a wall
    {
        if (ball.getX() - ball.radius < tableX + halfBorder)				// hit left wall
        {
            ball.setX(tableX + halfBorder + ball.radius);
            ball.velocityX = -ball.velocityX * bounce;
        }
        if (ball.getX() + ball.radius > screenW - halfBorder)				// hit right wall
        {
            ball.setX(screenW - halfBorder - ball.radius);
            ball.velocityX = -ball.velocityX * bounce;
        }
        if (ball.getY() - ball.radius < halfBorder)				// hit top wall
        {
            ball.setY(halfBorder + ball.radius);
            ball.velocityY = -ball.velocityY * bounce;
        }
        if (ball.getY() + ball.radius > screenH - halfBorder)				// hit bottom wall
        {
            ball.setY(screenH - halfBorder - ball.radius);
            ball.velocityY = -ball.velocityY * bounce;
        }
    }

    void ballCollision(Ball &a, Ball &b)				// elastic collision — swaps velocity components along the hit axis
    {
        double dx = b.getX() - a.getX();
        double dy = b.getY() - a.getY();
        double dist = sqrt(dx * dx + dy * dy);

        if (dist < 0.001)				// balls are basically on top of each other — nudge one aside
        {
            b.setX(b.getX() + 0.1);
            return;
        }

        double nx = dx / dist;				// unit normal pointing from a to b (the collision axis)
        double ny = dy / dist;

        double relVx = b.velocityX - a.velocityX;				// relative velocity between the two balls
        double relVy = b.velocityY - a.velocityY;

        double impact = relVx * nx + relVy * ny;				// how fast they're approaching each other

        if (impact > 0)				// they're already separating — nothing to do
            return;

        double impulse = -(1.0 + bounce) * impact / 2.0;				// force to apply to each ball

        a.velocityX = a.velocityX - impulse * nx;				// push ball a away
        a.velocityY = a.velocityY - impulse * ny;
        b.velocityX = b.velocityX + impulse * nx;				// push ball b away in the opposite direction
        b.velocityY = b.velocityY + impulse * ny;
    }

    bool areTouching(Ball &a, Ball &b)				// true if the two balls are overlapping
    {
        double dx = b.getX() - a.getX();
        double dy = b.getY() - a.getY();
        double dist = sqrt(dx * dx + dy * dy);
        return dist < (a.radius + b.radius);				// touching when centers are closer than combined radii
    }

    bool ballOverHole(Ball &ball, Hole &hole)				// true if ball is close enough to fall into the pocket
    {
        double dx = hole.getX() - ball.getX();
        double dy = hole.getY() - ball.getY();
        double dist = sqrt(dx * dx + dy * dy);
        return dist < (hole.getRadius() + ball.radius);				// overlap means potted
    }
};

class Rules
{
private:
    double totalTime;				// how many seconds the player gets in total
    double elapsed;				// how many seconds have passed since the first shot
    int shots;
    int ballsPotted;				// counts every colored ball that successfully drops into a pocket
    bool won;				// true only when the player clears the table correctly
    string playerName;

public:
    Rules(double total, string player)				// set up a fresh game with given time limit and player name
    {
        totalTime = total;
        elapsed = 0;
        shots = 0;
        ballsPotted = 0;				// no balls potted at the start of the game
        won = false;				// player has not won yet
        playerName = player;
    }

    void update(double dt)				// only tick the clock after the first shot is taken
    {
        if (shots > 0)
        {
            elapsed = elapsed + dt;
        }
    }

    double getTimeLeft()				// seconds remaining — clamps at 0 so it never goes negative
    {
        double left = totalTime - elapsed;
        if (left < 0)
            left = 0;
        return left;
    }

    bool isTimeUp() { return elapsed >= totalTime; }				// true when the clock runs out
    void addShot() { shots++; }				// call this each time the player fires the cue ball
    void addPenalty() { elapsed = elapsed + 10; }				// potting the cue ball costs 10 seconds
    void addPotted() { ballsPotted++; }				// call this every time a colored ball drops into a pocket
    void setWon(bool w) { won = w; }				// called once when the game ends — true for win, false for loss
    int getShots() { return shots; }
    string getPlayer() { return playerName; }

    double getScore()				// higher score for more time left and fewer shots taken
    {
        double potBonus    = ballsPotted * 20.0;				// 20 points for every ball successfully potted
        double winBonus    = won ? 300.0 : 0;				// big bonus for actually clearing the table correctly
        double timeBonus   = won ? (getTimeLeft() / totalTime) * 200.0 : 0;				// up to 200 points for speed — only rewarded on a win
        double shotPenalty = shots * 1.0;				// lose 1 point per shot taken
        double score = winBonus + timeBonus + potBonus - shotPenalty + 100.0;				// base 100 so even slow players get something
        if (score < 0)
            score = 0;
        return score;
    }

    bool checkWin(Ball **balls, int count)				// win = pink gone AND every other ball also gone
    {
        bool pinkGone = false;
        bool allGone = true;

        for (int i = 0; i < count; i++)
        {
            if (balls[i]->getColor() == pink)
            {
                pinkGone = balls[i]->is_gone();				// check if the special pink ball was potted
            }
            else
            {
                if (!balls[i]->is_gone())				// found a non-pink ball still on the table
                {
                    allGone = false;
                }
            }
        }

        return pinkGone && allGone;				// both conditions must be true to win
    }

    bool checkLoss(Ball **balls, int count)				// loss = pink potted while other balls are still out there
    {
        bool pinkGone = false;

        for (int i = 0; i < count; i++)
        {
            if (balls[i]->getColor() == pink && balls[i]->is_gone())
            {
                pinkGone = true;
                break;
            }
        }

        if (!pinkGone)				// pink is still on the table — can't have lost yet
            return false;

        for (int i = 0; i < count; i++)				// pink is gone — check if any other ball remains
        {
            if (balls[i]->getColor() != pink && !balls[i]->is_gone())
            {
                return true;				// yep, potted pink too early — game over
            }
        }

        return false;
    }
};

class ScoreManager
{
private:
    string fileName;				// the localStorage key we store scores under

public:
    ScoreManager()
    {
        fileName = "pool9ball";				// identifier used in browser localStorage
    }

    void save(string name, int score)				// persist the player's score to localStorage
    {
        fileSaveScore(name.c_str(), score);
    }

    void displayTopScores()				// draw the leaderboard on the left panel
    {
        int count = fileGetCount();

        drawText("Top Scorers:", 50 * scale, 58 * scale,
                 white.c_str(), (int)(26 * scale), 1);				// bold header

        for (int i = 0; i < count; i++)
        {
            char nameBuf[64] = {};
            fileGetName(i, nameBuf, 64);				// read name into C++ buffer
            int scoreVal = fileGetScore(i);

            PlayerScore ps(string(nameBuf), scoreVal);				// wrap in a PlayerScore so we can use its operators
            if (ps > PlayerScore())				// only display entries that beat the default empty entry
            {
                string entry = string(nameBuf) + "  -  " + to_string(scoreVal);				// "Alice  -  250"
                drawText(entry.c_str(),
                         50 * scale,
                         (96.0 + i * 30.0) * scale,				// space each entry 30px apart
                         white.c_str(),
                         (int)(20 * scale), 0);
            }
        }
    }
};

class Renderer
{
public:
    virtual ~Renderer() {}

    virtual void drawGame(
        bool aiming, double mouseX, double mouseY,
        CueBall *cue, Ball **balls, int numBalls,
        Hole **holes, int numHoles,
        Rules &rules, bool showFoul) = 0;				// subclasses handle the in-game view

    virtual void drawGameOver(
        Ball **balls, int numBalls,
        Rules &rules, ScoreManager &scores) = 0;				// subclasses handle the end screen
};

class CanvasRenderer : public Renderer
{
private:
    void drawCentered(string text, double y, string color, int size, bool bold)				// center text on the table area
    {
        double w = measureText(text.c_str(), size, bold ? 1 : 0);				// measure first so we can offset
        double x = table_x + (screen_w - table_x) / 2.0 - w / 2.0;				// center within the table area
        drawText(text.c_str(), x, y, color.c_str(), size, bold ? 1 : 0);
    }

public:
    void drawGame(
        bool aiming, double mouseX, double mouseY,
        CueBall *cue, Ball **balls, int numBalls,
        Hole **holes, int numHoles,
        Rules &rules, bool showFoul) override
    {
        drawRect(0, 0, screen_w, screen_h, felt_green.c_str());				// green felt background
        drawRect(0, 0, table_x, screen_h, dark_gray.c_str());				// dark left panel for scores

        drawRectOutline(				// wooden brown border around the table
            table_x + border_w / 2,
            border_w / 2,
            (screen_w - table_x) - border_w,
            screen_h - border_w,
            brown.c_str(), border_w);

        double baulkX = (1800.0 - 450.0) / 4.0 * scale + table_x;				// x position of the baulk line
        double baulkY = 900.0 / 2.0 * scale;				// vertical midpoint of the table
        drawLine(baulkX, 0, baulkX, screen_h, white.c_str(), baulk_w);				// vertical baulk line

        drawArc(baulkX, baulkY, 75.0 * scale,				// D-shaped semicircle on the baulk line
                -3.14159 / 2.0, 3.14159 / 2.0,
                white.c_str(), baulk_w);

        for (int i = 0; i < numHoles; i++)				// draw pockets first so balls appear on top
            holes[i]->draw();
        cue->draw();				// draw cue ball above holes
        for (int i = 0; i < numBalls; i++)				// draw colored balls on top of everything
            balls[i]->draw();

        if (aiming)				// show the aiming guide while the player is dragging
        {
            drawLine(cue->getX(), cue->getY(), mouseX, mouseY,
                     light_gray.c_str(), 1.5);				// shot direction line
            drawLine(cue->getX(), cue->getY(),
                     2 * cue->getX() - mouseX, 2 * cue->getY() - mouseY,
                     white.c_str(), 1.5);				// cue stick line (opposite direction)
        }

        int sz = (int)(17 * scale);
        drawText("Game Info",
                 50 * scale, 50 * scale,
                 red.c_str(), (int)(30 * scale), 1);				// bold red header on the left panel

        if (rules.getShots() == 0)				// no shots yet — show the how-to-play instructions
        {
            string lines[13] = {
                "HOW TO PLAY:", "",
                "Click on cue ball and",
                "drag, then release to shoot.", "",
                "RULES:", "",
                "* Pot all balls first.",
                "* Pot pink ball LAST to win.",
                "* Pink early = instant LOSS.",
                "* Cue ball potted = -10 sec.", "",
                "Player: " + rules.getPlayer()};
            double y = 78 * scale;
            for (int i = 0; i < 13; i++)
            {
                y += sz * 1.75;				// push each line down a bit
                drawText(lines[i].c_str(), 18 * scale, y, white.c_str(), sz, 0);
            }
        }
        else				// game is in progress — show live stats instead of instructions
        {
            drawText(("Player: " + rules.getPlayer()).c_str(),
                     20 * scale, 100 * scale, white.c_str(), sz + 2, 0);
            drawText(("Shots:  " + to_string(rules.getShots())).c_str(),
                     20 * scale, 100 * scale + (sz + 2) * 2.2, white.c_str(), sz + 2, 0);
        }

        if (showFoul)				// big FOUL message in the middle of the table for 2 seconds
        {
            int fz = (int)(44 * scale);
            drawCentered("FOUL", screen_h / 2 - 40 * scale, black, fz, true);
            drawCentered("-10 seconds!", screen_h / 2 + 28 * scale, black, fz, true);
        }

        ostringstream oss;
        oss << fixed << setprecision(2) << "Time Left: " << rules.getTimeLeft();				// format to 2 decimal places
        drawText(oss.str().c_str(),
                 20 * scale, 22 * scale,
                 "#4488ff", (int)(20 * scale), 1);				// blue bold countdown timer
    }

    void drawGameOver(
        Ball **balls, int numBalls,
        Rules &rules, ScoreManager &scores) override
    {
        drawRect(table_x, 0, screen_w - table_x, screen_h, black.c_str());				// black out the table
        drawRect(0, 0, table_x, screen_h, dark_gray.c_str());				// keep the left panel visible

        string msg;
        if (rules.isTimeUp())				// clock ran out
        {
            msg = "YOU LOST ON TIME";
        }
        else if (rules.checkWin(balls, numBalls))				// potted everything in the right order
        {
            msg = "YOU WON!  CONGRATULATIONS!";
        }
        else				// potted pink too early
        {
            msg = "YOU POTTED pink EARLY - GAME OVER";
        }

        int msgSz = (int)(40 * scale);
        drawCentered(msg, 150 * scale, red, msgSz, true);				// big red outcome message

        ostringstream ss;
        ss << fixed << setprecision(1) << "SCORE: " << rules.getScore();				// one decimal place
        drawCentered(ss.str(), 215 * scale, yellow, (int)(34 * scale), true);				// yellow score below the message

        scores.displayTopScores();				// show the leaderboard on the left panel
    }
};

class Game
{
private:
    Physics physics;
    Rules rules;
    ScoreManager scores;
    Renderer *renderer;

    CueBall *cue;
    Ball **balls;
    Hole **holes;
    int numBalls;
    int numHoles;

    bool gameOver;
    bool showFoul;				// true while the foul banner is visible on screen
    double foulTimer;				// counts how long the foul message has been shown
    bool allStopped;				// true when every single ball has come to rest
    bool scoreSaved;				// ensures we only write to localStorage once at the end
    bool aiming;				// true while the player is holding the mouse button down
    double mouseX;
    double mouseY;
    double lastTime;				// timestamp from the previous animation frame

public:
    Game(string playerName)
        : physics(table_x, screen_w, screen_h, border_w), rules(300.0, playerName)				// 5 minute time limit
    {
        renderer = new CanvasRenderer();

        cue = nullptr;
        balls = nullptr;
        holes = nullptr;
        numBalls = 0;
        numHoles = 0;

        gameOver = false;
        showFoul = false;
        foulTimer = 0;
        allStopped = true;
        scoreSaved = false;
        aiming = false;
        mouseX = 0;
        mouseY = 0;
        lastTime = -1;				// -1 means "no previous frame exists yet"

        setupGame();
        startMusic();

        emscripten_set_mousedown_callback("#gameCanvas", this, 1, onMouseDown);				// start aiming on click
        emscripten_set_mouseup_callback("#gameCanvas", this, 1, onMouseUp);				// fire shot on release
        emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, this, 1, onMouseMove);				// track mouse everywhere

        emscripten_request_animation_frame_loop(gameLoop, this);				// kick off the 60fps game loop
    }

    ~Game()				// free every ball, hole, cue, and renderer we allocated
    {
        for (int i = 0; i < numBalls; i++)
        {
            delete balls[i];
        }
        delete[] balls;

        for (int i = 0; i < numHoles; i++)
        {
            delete holes[i];
        }
        delete[] holes;

        delete cue;
        delete renderer;
    }

    Game(const Game &) = delete;				// no copying — would cause double-free bugs
    Game &operator=(const Game &) = delete;

private:
    void setupGame()				// place all balls and pockets in their starting positions
    {
        double sf = 1.1 * scale;				// spacing factor for the rack — a bit more than ball diameter
        double sx = (1800.0 - (1800.0 - 450.0) / 4.0) * scale;				// rack starts on the right side
        double sy = 900.0 / 2.0 * scale;				// vertically centered

        cue = new CueBall(				// cue ball starts on the left side of the baulk line
            (1800.0 - 450.0) / 4.0 * scale + table_x,
            sy,
            ball_radius);

        numBalls = 9;
        balls = new Ball *[numBalls];

        balls[0] = new Ball(sx - 60 * sf, sy, ball_radius, yellow);				// front of the rack
        balls[1] = new Ball(sx - 20 * sf, sy + 75 * sf, ball_radius, blue);				// bottom row
        balls[2] = new Ball(sx + 20 * sf, sy, ball_radius, red);				// back of the rack
        balls[3] = new Ball(sx - 40 * sf, sy - 37 * sf, ball_radius, purple);				// upper-left
        balls[4] = new Ball(sx, sy - 37 * sf, ball_radius, orange);				// upper-right
        balls[5] = new Ball(sx - 20 * sf, sy - 75 * sf, ball_radius, dark_green);				// top row
        balls[6] = new Ball(sx - 40 * sf, sy + 37 * sf, ball_radius, brown);				// lower-left
        balls[7] = new Ball(sx, sy + 37 * sf, ball_radius, black);				// lower-right
        balls[8] = new Ball(sx - 20 * sf, sy, ball_radius, pink);				// pink sits in the middle — must be potted last

        double bw = border_w;
        numHoles = 6;
        holes = new Hole *[numHoles];

        holes[0] = new Hole(table_x + bw / 2 + 3 * scale, bw / 2 + 3 * scale, hole_radius);				// top-left pocket
        holes[1] = new Hole(table_x + (screen_w - table_x) / 2, bw / 2 + 3 * scale, hole_radius);				// top-middle pocket
        holes[2] = new Hole(screen_w - bw / 2 - 3 * scale, bw / 2 + 3 * scale, hole_radius);				// top-right pocket
        holes[3] = new Hole(table_x + bw / 2 + 3 * scale, screen_h - bw / 2 - 3 * scale, hole_radius);				// bottom-left pocket
        holes[4] = new Hole(table_x + (screen_w - table_x) / 2, screen_h - bw / 2 - 3 * scale, hole_radius);				// bottom-middle pocket
        holes[5] = new Hole(screen_w - bw / 2 - 3 * scale, screen_h - bw / 2 - 3 * scale, hole_radius);				// bottom-right pocket
    }

    void update(double dt)				// advance the game by one time step
    {
        if (gameOver)				// once finished, just save the score once and bail out
        {
            if (!scoreSaved)
            {
                scores.save(rules.getPlayer(), (int)rules.getScore());
                scoreSaved = true;
            }
            return;
        }

        rules.update(dt);				// tick the game clock
        if (rules.isTimeUp())				// player ran out of time
        {
            gameOver = true;
            showGameOverButtons();
            return;
        }

        if (showFoul)				// count down the foul banner — hide it after 2 seconds
        {
            foulTimer = foulTimer + dt;
            if (foulTimer >= 2.0)
            {
                showFoul = false;
                foulTimer = 0;
            }
        }

        physics.moveBall(*cue, dt);				// slide the cue ball forward and apply friction
        physics.wallBounce(*cue);				// bounce it off any wall it hit

        for (int i = 0; i < numBalls; i++)				// do the same for every colored ball
        {
            physics.moveBall(*balls[i], dt);
            if (!balls[i]->is_gone())
            {
                physics.wallBounce(*balls[i]);				// only bounce balls still on the table
            }
        }

        for (int i = 0; i < numBalls; i++)				// check for cue ball hitting colored balls
        {
            if (!balls[i]->is_gone())
            {
                if (physics.areTouching(*cue, *balls[i]))
                {
                    physics.ballCollision(*cue, *balls[i]);				// resolve the collision
                    playHitSound();
                }
            }
            for (int j = i + 1; j < numBalls; j++)				// check colored balls hitting each other
            {
                if (!balls[i]->is_gone() && !balls[j]->is_gone())
                {
                    if (physics.areTouching(*balls[i], *balls[j]))
                    {
                        physics.ballCollision(*balls[i], *balls[j]);
                        playHitSound();
                    }
                }
            }
        }

        for (int i = 0; i < numHoles; i++)				// check if cue ball fell into a pocket (foul)
        {
            if (physics.ballOverHole(*cue, *holes[i]))
            {
                cue->resetTo(cue->getSpawnX(), cue->getSpawnY());				// respawn it at the start spot
                rules.addPenalty();				// dock 10 seconds from remaining time
                showFoul = true;
                foulTimer = 0;
                break;
            }
        }

        for (int i = 0; i < numBalls; i++)				// check if any colored ball fell into a pocket
        {
            if (!balls[i]->is_gone())
            {
                for (int j = 0; j < numHoles; j++)
                {
                    if (physics.ballOverHole(*balls[i], *holes[j]))
                    {
                        balls[i]->setGone(true);				// mark as potted
                        if (balls[i]->getColor() != pink)				// pink is a win/loss trigger, not a scoring bonus
                            rules.addPotted();				// record the pot so the score reflects it
                        playPot();
                        balls[i]->setX(-9999);				// move it off-screen so it can't collide
                        balls[i]->setY(-9999);
                        balls[i]->velocityX = 0;
                        balls[i]->velocityY = 0;
                        balls[i]->set_moving(false);
                        break;
                    }
                }
            }
        }

        allStopped = !cue->is_moving();				// start by assuming the cue ball is still
        if (allStopped)
        {
            for (int i = 0; i < numBalls; i++)				// if any ball is still rolling, we're not done
            {
                if (balls[i]->is_moving())
                {
                    allStopped = false;
                    break;
                }
            }
        }

        if (rules.checkWin(balls, numBalls))				// check win/loss after physics
        {
            rules.setWon(true);				// player cleared the table correctly
            gameOver = true;
            showGameOverButtons();
        }
        else if (rules.checkLoss(balls, numBalls))
        {
            rules.setWon(false);				// player potted pink too early
            gameOver = true;
            showGameOverButtons();
        }
    }

    void draw()				// pick the right screen to render based on game state
    {
        if (!gameOver)
        {
            renderer->drawGame(
                aiming, mouseX, mouseY,
                cue, balls, numBalls,
                holes, numHoles,
                rules, showFoul);				// draw the live gameplay view
        }
        else
        {
            renderer->drawGameOver(balls, numBalls, rules, scores);				// draw the results screen
        }
    }

    static EM_BOOL gameLoop(double timestamp, void *userData)				// called ~60 times per second by the browser
    {
        Game *game = (Game *)userData;

        if (game->lastTime < 0)				// very first frame — just record the time and skip
        {
            game->lastTime = timestamp;
        }

        double dt = (timestamp - game->lastTime) / 1000.0;				// convert milliseconds to seconds
        if (dt > 0.1)				// cap the time step so a hidden tab doesn't cause a huge jump
            dt = 0.1;
        game->lastTime = timestamp;

        game->update(dt);
        game->draw();

        return EM_TRUE;				// keep the loop running every frame
    }

    static EM_BOOL onMouseDown(int type, const EmscriptenMouseEvent *e, void *userData)				// player presses mouse
    {
        Game *game = (Game *)userData;

        if (!game->gameOver && !game->cue->is_moving() && game->allStopped)				// only allow aiming when everything is still
        {
            game->mouseX = e->targetX * getscaleX();				// scale click position to canvas resolution
            game->mouseY = e->targetY * getscaleY();
            game->aiming = true;
        }

        return EM_TRUE;
    }

    static EM_BOOL onMouseUp(int type, const EmscriptenMouseEvent *e, void *userData)				// player releases mouse — fire the shot
    {
        Game *game = (Game *)userData;

        if (game->aiming && !game->gameOver)
        {
            double mouseX = e->targetX * getscaleX();
            double mouseY = e->targetY * getscaleY();

            game->cue->velocityX = (game->cue->getX() - mouseX) * cue_power;				// pull-back direction = shot direction
            game->cue->velocityY = (game->cue->getY() - mouseY) * cue_power;
            game->cue->set_moving(true);
            playHit();				// play the cue strike sound
            game->rules.addShot();				// count this as a shot
            game->aiming = false;
            game->allStopped = false;
        }

        return EM_TRUE;
    }

    static EM_BOOL onMouseMove(int type, const EmscriptenMouseEvent *e, void *userData)				// update aiming line while dragging
    {
        Game *game = (Game *)userData;

        if (game->aiming){
            double canvasLeft = getCanvasLeft();
            double canvasTop = getCanvasTop();
            game->mouseX = (e->clientX - canvasLeft) * getscaleX();				// convert screen coords to canvas coords
            game->mouseY = (e->clientY - canvasTop) * getscaleY();
        }

        return EM_TRUE;
    }
};

static Game *theGame = nullptr;				// one global game instance — only one game runs at a time

extern "C"
{
    EMSCRIPTEN_KEEPALIVE
    void startGame(const char *playerName){				// called from JavaScript to kick off a fresh game
        delete theGame;				// clean up any previous game first
        theGame = new Game(string(playerName));
    }
}