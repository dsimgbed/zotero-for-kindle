# color.pdf: a page with color on it, for the Colorsoft tests (pages rendered in color). Run:
#   uv run --with reportlab python -u tests/data/pdf/make_color.py
# One letter page: a red square (top left), a blue bar, and black text lines below them (where
# the tests put highlights in Zotero's colours).
from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas

c = canvas.Canvas("tests/data/pdf/color.pdf", pagesize=letter)
c.setFillColorRGB(0.9, 0.1, 0.1)
c.rect(72, 560, 160, 160, stroke=0, fill=1)
c.setFillColorRGB(0.1, 0.3, 0.9)
c.rect(260, 640, 280, 80, stroke=0, fill=1)
c.setFillColorRGB(0, 0, 0)
c.setFont("Helvetica", 14)
lines = [
    "Physics-informed neural networks solve the Hamilton-Jacobi-Bellman",
    "equation of a consumption-savings problem on a grid of wealth and",
    "income. The value function is concave and its derivative is the",
    "marginal utility of consumption. Highlights in Zotero's colours",
    "are drawn over these lines: yellow, red, green, blue and purple,",
    "and an underline in orange below the last of them.",
]
y = 500
for line in lines:
    c.drawString(72, y, line)
    y -= 28
c.showPage()
c.save()
